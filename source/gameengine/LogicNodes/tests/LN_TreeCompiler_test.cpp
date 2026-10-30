/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "DNA_ID_enums.h"
#include "DNA_collection_types.h"
#include "DNA_color_types.h"
#include "DNA_logic_node_binding_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_rigidbody_types.h"

#include "SCA_IInputDevice.h"
#include "SCA_InputEvent.h"

#include "BKE_colortools.hh"

#include "LN_BindingSource.h"
#include "LN_CommandBuffer.h"
#include "LN_RuntimeTree.h"
#include "LN_TreeCompiler.h"
#include "DEV_InputDevice.h"
#include "KX_Camera.h"
#include "KX_GameObject.h"
#include "LN_Manager.h"
#include "LN_Snapshot.h"
#include "LN_UIService.h"
#include "SG_Node.h"

namespace {

template<size_t Size> void SetCString(char (&dst)[Size], const char *value)
{
  dst[0] = '\0';
  std::strncpy(dst, value, Size - 1);
  dst[Size - 1] = '\0';
}

template<typename T> void Append(blender::ListBaseT<T> &list, T &item)
{
  item.next = nullptr;
  item.prev = static_cast<T *>(list.last);
  if (list.last != nullptr) {
    static_cast<T *>(list.last)->next = &item;
  }
  else {
    list.first = &item;
  }
  list.last = &item;
}

template<typename T> void Remove(blender::ListBaseT<T> &list, T &item)
{
  if (item.prev != nullptr) {
    item.prev->next = item.next;
  }
  else {
    list.first = item.next;
  }
  if (item.next != nullptr) {
    item.next->prev = item.prev;
  }
  else {
    list.last = item.prev;
  }
  item.next = nullptr;
  item.prev = nullptr;
}

void InitTree(blender::bNodeTree &tree)
{
  SetCString(tree.id.name, "NTCompilerProbe");
  SetCString(tree.idname, "LogicNodeTree");
  tree.type = blender::NTREE_LOGIC;
}

void InitNode(blender::bNode &node, const char *idname, const char *name, int32_t identifier)
{
  SetCString(node.idname, idname);
  SetCString(node.name, name);
  node.identifier = identifier;
}

void InitMaterial(blender::Material &material, const char *name)
{
  *reinterpret_cast<uint16_t *>(material.id.name) = uint16_t(blender::ID_MA);
  std::strncpy(material.id.name + 2, name, sizeof(material.id.name) - 3);
  material.id.name[sizeof(material.id.name) - 1] = '\0';
}

void InitGeometryModifierGroup(blender::bNodeTree &tree,
                               blender::GeometryNodeAssetTraits &traits,
                               const char *name)
{
  *reinterpret_cast<uint16_t *>(tree.id.name) = uint16_t(blender::ID_NT);
  std::strncpy(tree.id.name + 2, name, sizeof(tree.id.name) - 3);
  tree.id.name[sizeof(tree.id.name) - 1] = '\0';
  tree.type = blender::NTREE_GEOMETRY;
  traits.flag = blender::GEO_NODE_ASSET_MODIFIER;
  tree.geometry_node_asset_traits = &traits;
}

void InitSocket(blender::bNodeSocket &socket,
                const char *name,
                blender::eNodeSocketDatatype type,
                void *default_value,
                const char *idname = nullptr)
{
  SetCString(socket.identifier, name);
  SetCString(socket.name, name);
  if (idname != nullptr) {
    SetCString(socket.idname, idname);
  }
  socket.type = type;
  socket.default_value = default_value;
}

void InitSocketWithIdentifier(blender::bNodeSocket &socket,
                              const char *identifier,
                              const char *name,
                              blender::eNodeSocketDatatype type,
                              void *default_value,
                              const char *idname = nullptr)
{
  SetCString(socket.identifier, identifier);
  SetCString(socket.name, name);
  if (idname != nullptr) {
    SetCString(socket.idname, idname);
  }
  socket.type = type;
  socket.default_value = default_value;
}

/* Object Grid View's inputs for its list (Object, Property) and its Moving Items, Drop Hints,
 * Filter and Use sections, at their defaults: Items gives the list, Moving Items is off. Declare it
 * as a local next to the grid node: the node points at it. */
struct GridListSockets {
  std::array<blender::bNodeSocket, 18> sockets = {};
  blender::bNodeSocketValueObject object = {};
  blender::bNodeSocketValueString property = {};
  blender::bNodeSocketValueBoolean handle_moves = {};
  blender::bNodeSocketValueString split_action = {};
  blender::bNodeSocketValueInt split_amount = {};
  blender::bNodeSocketValueBoolean swap = {};
  blender::bNodeSocketValueBoolean drop_into_world = {};
  blender::bNodeSocketValueBoolean drop_at_pointer = {};
  blender::bNodeSocketValueBoolean drop_in_front = {};
  blender::bNodeSocketValueFloat distance = {};
  blender::bNodeSocketValueFloat hover_height = {};
  blender::bNodeSocketValueObject hint_text = {};
  blender::bNodeSocketValueString accept_hint = {};
  blender::bNodeSocketValueString refuse_hint = {};
  blender::bNodeSocketValueString show_categories = {};
  blender::bNodeSocketValueString search = {};
  blender::bNodeSocketValueString use_action = {};
  blender::bNodeSocketValueString use_keys = {};

  void AppendTo(blender::bNode &grid)
  {
    swap.value = true;
    distance.value = 1.5f;
    SetCString(accept_hint.value, "Put {name} here");
    SetCString(refuse_hint.value, "{name} does not fit here");
    const std::array<std::tuple<const char *, blender::eNodeSocketDatatype, void *>, 18> inputs = {
        {{"Object", blender::SOCK_OBJECT, &object},
         {"Property", blender::SOCK_STRING, &property},
         {"Handle Moves", blender::SOCK_BOOLEAN, &handle_moves},
         {"Split Action", blender::SOCK_STRING, &split_action},
         {"Split Amount", blender::SOCK_INT, &split_amount},
         {"Swap", blender::SOCK_BOOLEAN, &swap},
         {"Drop Into World", blender::SOCK_BOOLEAN, &drop_into_world},
         {"At Pointer", blender::SOCK_BOOLEAN, &drop_at_pointer},
         {"Drop In Front", blender::SOCK_BOOLEAN, &drop_in_front},
         {"Distance", blender::SOCK_FLOAT, &distance},
         {"Hover Height", blender::SOCK_FLOAT, &hover_height},
         {"Hint Text", blender::SOCK_OBJECT, &hint_text},
         {"Accept Hint", blender::SOCK_STRING, &accept_hint},
         {"Refuse Hint", blender::SOCK_STRING, &refuse_hint},
         {"Show Categories", blender::SOCK_STRING, &show_categories},
         {"Search", blender::SOCK_STRING, &search},
         {"Use Action", blender::SOCK_STRING, &use_action},
         {"Use Keys", blender::SOCK_STRING, &use_keys}}};
    for (size_t index = 0; index < inputs.size(); ++index) {
      const auto &[name, type, value] = inputs[index];
      InitSocket(sockets[index], name, type, value);
      Append(grid.inputs, sockets[index]);
    }
  }

  blender::bNodeSocket &Input(const char *name)
  {
    for (blender::bNodeSocket &socket : sockets) {
      if (std::strcmp(socket.name, name) == 0) {
        return socket;
      }
    }
    return sockets.front();
  }
};

void InitLink(blender::bNodeLink &link,
              blender::bNode &from_node,
              blender::bNodeSocket &from_socket,
              blender::bNode &to_node,
              blender::bNodeSocket &to_socket)
{
  link.fromnode = &from_node;
  link.fromsock = &from_socket;
  link.tonode = &to_node;
  link.tosock = &to_socket;
}

struct MaterialParameterValueSockets {
  blender::bNodeSocket generic_value = {};
  blender::bNodeSocket float_value = {};
  blender::bNodeSocket integer_value = {};
  blender::bNodeSocket boolean_value = {};
  blender::bNodeSocket vector_value = {};
  blender::bNodeSocket color_value = {};
};

struct SetParentNodeSockets {
  blender::bNodeSocket flow = {};
  blender::bNodeSocket child = {};
  blender::bNodeSocket parent = {};
  blender::bNodeSocket bone_name = {};
  blender::bNodeSocket compound = {};
  blender::bNodeSocket ghost = {};
};

void InitSetParentNode(blender::bNode &node,
                       SetParentNodeSockets &sockets,
                       blender::bNodeSocketValueObject &child_default,
                       blender::bNodeSocketValueObject &parent_default,
                       blender::bNodeSocketValueString &bone_name_default,
                       blender::bNodeSocketValueBoolean &compound_default,
                       blender::bNodeSocketValueBoolean &ghost_default,
                       const int32_t identifier)
{
  InitNode(node, "LogicNativeSetParent", "Set Parent", identifier);
  InitSocket(sockets.flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(sockets.child, "Child Object", blender::SOCK_OBJECT, &child_default);
  InitSocket(sockets.parent, "Parent Object", blender::SOCK_OBJECT, &parent_default);
  InitSocket(sockets.bone_name, "Bone Name", blender::SOCK_STRING, &bone_name_default);
  InitSocket(sockets.compound, "Compound", blender::SOCK_BOOLEAN, &compound_default);
  InitSocket(sockets.ghost, "Ghost", blender::SOCK_BOOLEAN, &ghost_default);
  Append(node.inputs, sockets.flow);
  Append(node.inputs, sockets.child);
  Append(node.inputs, sockets.parent);
  Append(node.inputs, sockets.bone_name);
  Append(node.inputs, sockets.compound);
  Append(node.inputs, sockets.ghost);
}

void AppendMaterialParameterValueSockets(blender::bNode &node,
                                         MaterialParameterValueSockets &sockets,
                                         void *float_default,
                                         void *color_default)
{
  InitSocketWithIdentifier(sockets.generic_value, "Value", "Value", blender::SOCK_CUSTOM, nullptr);
  InitSocketWithIdentifier(sockets.float_value,
                           "Float Value",
                           "Value",
                           blender::SOCK_FLOAT,
                           float_default,
                           "NodeSocketLogicFloat");
  InitSocketWithIdentifier(sockets.integer_value,
                           "Integer Value",
                           "Value",
                           blender::SOCK_INT,
                           nullptr,
                           "NodeSocketLogicInt");
  InitSocketWithIdentifier(sockets.boolean_value,
                           "Boolean Value",
                           "Value",
                           blender::SOCK_BOOLEAN,
                           nullptr,
                           "NodeSocketLogicBool");
  InitSocketWithIdentifier(sockets.vector_value,
                           "Vector Value",
                           "Value",
                           blender::SOCK_VECTOR,
                           nullptr,
                           "NodeSocketLogicVector");
  InitSocketWithIdentifier(sockets.color_value,
                           "Color Value",
                           "Value",
                           blender::SOCK_RGBA,
                           color_default,
                           "NodeSocketLogicColor");
  Append(node.inputs, sockets.generic_value);
  Append(node.inputs, sockets.float_value);
  Append(node.inputs, sockets.integer_value);
  Append(node.inputs, sockets.boolean_value);
  Append(node.inputs, sockets.vector_value);
  Append(node.inputs, sockets.color_value);
}

struct SetObjectAttributeAuxSockets {
  blender::bNodeSocket xyz = {};
  blender::bNodeSocket object = {};
  blender::bNodeSocket position = {};
  blender::bNodeSocket rotation = {};
  blender::bNodeSocket scale = {};
  blender::bNodeSocket color = {};
  blender::bNodeSocket visible = {};
  blender::bNodeSocket include_children = {};
  blender::bNodeSocket teleport = {};
  blender::bNodeSocket done = {};
  blender::bNodeSocketValueVector xyz_default = {};
  blender::bNodeSocketValueBoolean teleport_default = {};
};

void InitSetObjectAttributeVectorNode(blender::bNode &node,
                                      SetObjectAttributeAuxSockets &aux,
                                      blender::bNodeSocket &flow,
                                      blender::bNodeSocket &value,
                                      const char *name,
                                      const int32_t identifier,
                                      const int attribute_type,
                                      void *value_default,
                                      const char *flow_identifier = "Flow",
                                      const char *flow_socket_idname = nullptr)
{
  InitNode(node, "LogicNativeSetObjectAttribute", name, identifier);
  node.custom1 = attribute_type;
  /* The editor initializes all three component-mask bits plus the initialized bit. */
  node.custom2 = 15;
  aux.xyz_default.value[0] = 1.0f;
  aux.xyz_default.value[1] = 1.0f;
  aux.xyz_default.value[2] = 1.0f;

  InitSocket(flow, flow_identifier, blender::SOCK_BOOLEAN, nullptr, flow_socket_idname);
  InitSocket(aux.xyz, "XYZ", blender::SOCK_VECTOR, &aux.xyz_default);
  InitSocket(aux.object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(value, "Value", blender::SOCK_VECTOR, value_default);
  InitSocket(aux.position, "Position", blender::SOCK_VECTOR, nullptr);
  InitSocket(aux.rotation, "Rotation", blender::SOCK_ROTATION, nullptr);
  InitSocket(aux.scale, "Scale", blender::SOCK_VECTOR, nullptr);
  InitSocket(aux.color, "Color", blender::SOCK_RGBA, nullptr);
  InitSocket(aux.visible, "Visible", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(aux.include_children, "Include Children", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(aux.teleport, "Teleport", blender::SOCK_BOOLEAN, &aux.teleport_default);
  InitSocket(aux.done, "Done", blender::SOCK_BOOLEAN, nullptr);

  Append(node.inputs, flow);
  Append(node.inputs, aux.xyz);
  Append(node.inputs, aux.object);
  Append(node.inputs, value);
  Append(node.inputs, aux.position);
  Append(node.inputs, aux.rotation);
  Append(node.inputs, aux.scale);
  Append(node.inputs, aux.color);
  Append(node.inputs, aux.visible);
  Append(node.inputs, aux.include_children);
  Append(node.inputs, aux.teleport);
  Append(node.outputs, aux.done);
}

struct GetObjectAttributeAuxSockets {
  blender::bNodeSocket object = {};
  blender::bNodeSocket name = {};
  blender::bNodeSocket visible = {};
  blender::bNodeSocket position = {};
  blender::bNodeSocket orientation = {};
  blender::bNodeSocket scale = {};
  blender::bNodeSocket color = {};
};

void InitGetObjectAttributeVectorNode(blender::bNode &node,
                                      GetObjectAttributeAuxSockets &aux,
                                      blender::bNodeSocket &vector,
                                      const char *name,
                                      const int32_t identifier,
                                      const int attribute_type)
{
  InitNode(node, "LogicNativeGetObjectAttribute", name, identifier);
  node.custom1 = attribute_type;
  InitSocket(aux.object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(aux.name, "Name", blender::SOCK_STRING, nullptr);
  InitSocket(vector, "Vector", blender::SOCK_VECTOR, nullptr);
  InitSocket(aux.visible, "Visible", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(aux.position, "Position", blender::SOCK_VECTOR, nullptr);
  InitSocket(aux.orientation, "Orientation", blender::SOCK_ROTATION, nullptr);
  InitSocket(aux.scale, "Scale", blender::SOCK_VECTOR, nullptr);
  InitSocket(aux.color, "Color", blender::SOCK_RGBA, nullptr);

  Append(node.inputs, aux.object);
  Append(node.outputs, aux.name);
  Append(node.outputs, vector);
  Append(node.outputs, aux.visible);
  Append(node.outputs, aux.position);
  Append(node.outputs, aux.orientation);
  Append(node.outputs, aux.scale);
  Append(node.outputs, aux.color);
}

bool HasFloatConstant(const LN_Program &program, float expected)
{
  for (const LN_Constant &constant : program.GetConstants()) {
    if (constant.value.type == LN_ValueType::Float &&
        std::abs(constant.value.float_value - expected) < 0.0001f)
    {
      return true;
    }
  }
  return false;
}

bool HasVectorInstruction(const std::vector<LN_Instruction> &instructions,
                          LN_OpCode opcode,
                          const MT_Vector3 &expected)
{
  for (const LN_Instruction &instruction : instructions) {
    if (instruction.opcode == opcode && (instruction.vector_value - expected).length() < 0.0001f) {
      return true;
    }
  }
  return false;
}

bool HasApplyImpulseInstruction(const std::vector<LN_Instruction> &instructions,
                                const MT_Vector3 &expected_attach,
                                const MT_Vector3 &expected_impulse)
{
  for (const LN_Instruction &instruction : instructions) {
    if (instruction.opcode == LN_OpCode::ApplyImpulse &&
        (instruction.secondary_vector_value - expected_attach).length() < 0.0001f &&
        (instruction.vector_value - expected_impulse).length() < 0.0001f)
    {
      return true;
    }
  }
  return false;
}

bool HasGamePropertyRef(const LN_Program &program,
                        const std::string &name,
                        LN_ValueType value_type)
{
  for (const LN_GamePropertyRef &property_ref : program.GetGamePropertyRefs()) {
    if (property_ref.name == name && property_ref.value_type == value_type) {
      return true;
    }
  }
  return false;
}

const LN_GamePropertyRef *GamePropertyRefAt(const LN_Program &program, uint32_t index)
{
  const std::vector<LN_GamePropertyRef> &property_refs = program.GetGamePropertyRefs();
  if (index == LN_INVALID_INDEX || index >= property_refs.size()) {
    return nullptr;
  }
  return &property_refs[index];
}

const LN_BoolExpression *BoolExpressionAt(const LN_Program &program, uint32_t index)
{
  const std::vector<LN_BoolExpression> &expressions = program.GetBoolExpressions();
  if (index == LN_INVALID_INDEX || index >= expressions.size()) {
    return nullptr;
  }
  return &expressions[index];
}

bool BoolExpressionTreeContainsKind(const LN_Program &program,
                                    const uint32_t index,
                                    const LN_BoolExpressionKind kind)
{
  const std::vector<LN_BoolExpression> &expressions = program.GetBoolExpressions();
  if (index == LN_INVALID_INDEX || index >= expressions.size()) {
    return false;
  }

  std::vector<uint32_t> stack = {index};
  std::vector<bool> visited(expressions.size(), false);
  while (!stack.empty()) {
    const uint32_t expr_index = stack.back();
    stack.pop_back();
    if (expr_index == LN_INVALID_INDEX || expr_index >= expressions.size() || visited[expr_index])
    {
      continue;
    }
    visited[expr_index] = true;
    const LN_BoolExpression &expression = expressions[expr_index];
    if (expression.kind == kind) {
      return true;
    }
    stack.push_back(expression.input0);
    stack.push_back(expression.input1);
    stack.push_back(expression.input2);
  }
  return false;
}

const LN_FloatExpression *FloatExpressionAt(const LN_Program &program, uint32_t index)
{
  const std::vector<LN_FloatExpression> &expressions = program.GetFloatExpressions();
  if (index == LN_INVALID_INDEX || index >= expressions.size()) {
    return nullptr;
  }
  return &expressions[index];
}

const LN_ColorExpression *ColorExpressionAt(const LN_Program &program, uint32_t index)
{
  const std::vector<LN_ColorExpression> &expressions = program.GetColorExpressions();
  if (index == LN_INVALID_INDEX || index >= expressions.size()) {
    return nullptr;
  }
  return &expressions[index];
}

const LN_IntExpression *IntExpressionAt(const LN_Program &program, uint32_t index)
{
  const std::vector<LN_IntExpression> &expressions = program.GetIntExpressions();
  if (index == LN_INVALID_INDEX || index >= expressions.size()) {
    return nullptr;
  }
  return &expressions[index];
}

const LN_ValueExpression *ValueExpressionAt(const LN_Program &program, uint32_t index)
{
  const std::vector<LN_ValueExpression> &expressions = program.GetValueExpressions();
  if (index == LN_INVALID_INDEX || index >= expressions.size()) {
    return nullptr;
  }
  return &expressions[index];
}

const LN_Instruction *FindInstruction(const std::vector<LN_Instruction> &instructions,
                                      LN_OpCode opcode)
{
  for (const LN_Instruction &instruction : instructions) {
    if (instruction.opcode == opcode) {
      return &instruction;
    }
  }
  return nullptr;
}

const LN_Instruction *FindBranchRouteWithConditionKind(
    const LN_Program &program,
    const std::vector<LN_Instruction> &instructions,
    const LN_BoolExpressionKind kind)
{
  for (const LN_Instruction &instruction : instructions) {
    if (instruction.opcode == LN_OpCode::BranchRoute &&
        BoolExpressionTreeContainsKind(program, instruction.bool_expr_index, kind))
    {
      return &instruction;
    }
  }
  return nullptr;
}

void ExpectInstructionGuardedByRouteCondition(const LN_Program &program,
                                              const LN_Event event,
                                              const LN_Instruction &instruction,
                                              const LN_BoolExpressionKind condition_kind)
{
  const std::vector<LN_Instruction> &instructions = program.GetInstructions(event);
  const LN_Instruction *route = FindBranchRouteWithConditionKind(
      program, instructions, condition_kind);
  ASSERT_NE(route, nullptr);
  const uint32_t route_index = uint32_t(route - instructions.data());

  const LN_BoolExpression *guard = BoolExpressionAt(program, instruction.bool_guard_expr_index);
  ASSERT_NE(guard, nullptr);
  EXPECT_EQ(guard->kind, LN_BoolExpressionKind::InstructionExecuted);
  EXPECT_EQ(guard->input0, route_index);
}

int FindInstructionIndex(const std::vector<LN_Instruction> &instructions, LN_OpCode opcode)
{
  for (size_t index = 0; index < instructions.size(); index++) {
    if (instructions[index].opcode == opcode) {
      return int(index);
    }
  }
  return -1;
}

const LN_VectorExpression *InstructionVectorExpression(const LN_Program &program,
                                                       const LN_Instruction &instruction)
{
  if (instruction.vector_expr_index == LN_INVALID_INDEX) {
    return nullptr;
  }

  const std::vector<LN_VectorExpression> &expressions = program.GetVectorExpressions();
  if (instruction.vector_expr_index >= expressions.size()) {
    return nullptr;
  }
  return &expressions[instruction.vector_expr_index];
}

const LN_VectorExpression *VectorExpressionAt(const LN_Program &program, uint32_t index)
{
  const std::vector<LN_VectorExpression> &expressions = program.GetVectorExpressions();
  if (index == LN_INVALID_INDEX || index >= expressions.size()) {
    return nullptr;
  }
  return &expressions[index];
}

const LN_StringExpression *StringExpressionAt(const LN_Program &program, const uint32_t index)
{
  const std::vector<LN_StringExpression> &expressions = program.GetStringExpressions();
  if (index == LN_INVALID_INDEX || index >= expressions.size()) {
    return nullptr;
  }
  return &expressions[index];
}

void ExpectObjectAttributeVectorExpression(const LN_Program &program,
                                           const LN_VectorExpression &expression,
                                           const char *attribute_name)
{
  EXPECT_EQ(expression.kind, LN_VectorExpressionKind::FromGenericValue);
  const LN_ValueExpression *value_expression = ValueExpressionAt(program, expression.input0);
  ASSERT_NE(value_expression, nullptr);
  EXPECT_EQ(value_expression->kind, LN_ValueExpressionKind::ObjectAttribute);
  const LN_StringExpression *attribute_expression = StringExpressionAt(program,
                                                                       value_expression->input1);
  ASSERT_NE(attribute_expression, nullptr);
  EXPECT_EQ(attribute_expression->kind, LN_StringExpressionKind::Constant);
  EXPECT_EQ(attribute_expression->string_value, attribute_name);
}

TEST(LN_TreeCompiler, EmitsEventInstructionsAndTopologicalConstants)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueFloat value_default = {};
  value_default.value = 2.5f;
  blender::bNode value = {};
  blender::bNodeSocket value_output = {};
  InitNode(value, "LogicNativeValueFloat", "Value", 2);
  InitSocket(value_output, "Float", blender::SOCK_FLOAT, &value_default);
  Append(value.outputs, value_output);
  Append(tree.nodes, value);

  blender::bNodeSocketValueFloat math_a_default = {};
  blender::bNodeSocketValueFloat math_b_default = {};
  math_b_default.value = 3.0f;
  blender::bNode math = {};
  blender::bNodeSocket math_a = {};
  blender::bNodeSocket math_b = {};
  blender::bNodeSocket math_result = {};
  InitNode(math, "LogicNativeMath", "Math", 3);
  math.custom1 = blender::NODE_MATH_ADD;
  InitSocket(math_a, "A", blender::SOCK_FLOAT, &math_a_default);
  InitSocket(math_b, "B", blender::SOCK_FLOAT, &math_b_default);
  InitSocket(math_result, "Result", blender::SOCK_FLOAT, nullptr);
  Append(math.inputs, math_a);
  Append(math.inputs, math_b);
  Append(math.outputs, math_result);
  Append(tree.nodes, math);

  blender::bNodeSocketValueFloat timescale_default = {};
  timescale_default.value = 1.0f;
  blender::bNode set_timescale = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket timescale = {};
  InitNode(set_timescale, "LogicNativeSetTimescale", "Set Timescale", 4);
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(timescale, "Timescale", blender::SOCK_FLOAT, &timescale_default);
  Append(set_timescale.inputs, flow);
  Append(set_timescale.inputs, timescale);
  Append(tree.nodes, set_timescale);

  blender::bNodeLink value_to_math = {};
  blender::bNodeLink math_to_timescale = {};
  blender::bNodeLink event_to_set_timescale = {};
  InitLink(value_to_math, value, value_output, math, math_a);
  InitLink(math_to_timescale, math, math_result, set_timescale, timescale);
  InitLink(event_to_set_timescale, on_update, on_update_pulse, set_timescale, flow);
  Append(tree.links, value_to_math);
  Append(tree.links, math_to_timescale);
  Append(tree.links, event_to_set_timescale);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors());
  EXPECT_EQ(program->GetInstructions(LN_Event::OnUpdate).size(), 2);
  EXPECT_TRUE(HasFloatConstant(*program, 2.5f));
  EXPECT_TRUE(HasFloatConstant(*program, 5.5f));

  const std::vector<int32_t> &order = program->GetSourceNodeOrder();
  const auto value_iter = std::find(order.begin(), order.end(), 2);
  const auto math_iter = std::find(order.begin(), order.end(), 3);
  ASSERT_NE(value_iter, order.end());
  ASSERT_NE(math_iter, order.end());
  EXPECT_LT(value_iter, math_iter);
}

TEST(LN_TreeCompiler, RejectsCycles)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueFloat a_default = {};
  blender::bNodeSocketValueFloat b_default = {};
  blender::bNode first = {};
  blender::bNodeSocket first_a = {};
  blender::bNodeSocket first_b = {};
  blender::bNodeSocket first_result = {};
  InitNode(first, "LogicNativeMath", "First", 2);
  InitSocket(first_a, "A", blender::SOCK_FLOAT, &a_default);
  InitSocket(first_b, "B", blender::SOCK_FLOAT, &b_default);
  InitSocket(first_result, "Result", blender::SOCK_FLOAT, nullptr);
  Append(first.inputs, first_a);
  Append(first.inputs, first_b);
  Append(first.outputs, first_result);
  Append(tree.nodes, first);

  blender::bNode second = {};
  blender::bNodeSocket second_a = {};
  blender::bNodeSocket second_b = {};
  blender::bNodeSocket second_result = {};
  InitNode(second, "LogicNativeMath", "Second", 3);
  InitSocket(second_a, "A", blender::SOCK_FLOAT, &a_default);
  InitSocket(second_b, "B", blender::SOCK_FLOAT, &b_default);
  InitSocket(second_result, "Result", blender::SOCK_FLOAT, nullptr);
  Append(second.inputs, second_a);
  Append(second.inputs, second_b);
  Append(second.outputs, second_result);
  Append(tree.nodes, second);

  blender::bNodeSocketValueVector position_default = {};
  blender::bNode set_position = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket position = {};
  SetObjectAttributeAuxSockets set_attribute_aux = {};
  InitSetObjectAttributeVectorNode(
      set_position, set_attribute_aux, flow, position, "Set Position", 4, 0, &position_default);
  Append(tree.nodes, set_position);

  blender::bNodeLink first_to_second = {};
  blender::bNodeLink second_to_first = {};
  blender::bNodeLink math_to_position = {};
  blender::bNodeLink event_to_set_position = {};
  InitLink(first_to_second, first, first_result, second, second_a);
  InitLink(second_to_first, second, second_result, first, first_a);
  InitLink(math_to_position, second, second_result, set_position, position);
  InitLink(event_to_set_position, on_update, on_update_pulse, set_position, flow);
  Append(tree.links, first_to_second);
  Append(tree.links, second_to_first);
  Append(tree.links, math_to_position);
  Append(tree.links, event_to_set_position);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_TRUE(program->GetCompileReport().HasErrors());
}

/* Set Object Attribute's Teleport: a reset of Smooth Motion right after the write when it is
 * checked, nothing at all when it is not (no cost), and none for velocities (not a jump). */
TEST(LN_TreeCompiler, SetObjectAttributeTeleportResetsSmoothingAfterTheWrite)
{
  struct Case {
    int attribute_type;
    bool teleport;
    /** The write as the compiler lowers it. */
    LN_OpCode write;
    bool reset_expected;
  };
  const Case cases[] = {
      {0, false, LN_OpCode::SetTransformVector, false},
      {0, true, LN_OpCode::SetTransformVector, true},
      {6, true, LN_OpCode::SetTransformVector, true},
      {5, true, LN_OpCode::SetTransformVector, true},
      {2, true, LN_OpCode::SetVelocityVector, false},
  };
  for (const Case &test_case : cases) {
    blender::bNodeTree tree = {};
    InitTree(tree);

    blender::bNode on_update = {};
    blender::bNodeSocket on_update_pulse = {};
    InitNode(on_update, "LogicNativeOnFixedUpdate", "On Fixed Update", 1);
    InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
    Append(on_update.outputs, on_update_pulse);
    Append(tree.nodes, on_update);

    blender::bNodeSocketValueVector value_default = {};
    blender::bNode set_attribute = {};
    blender::bNodeSocket flow = {};
    blender::bNodeSocket value = {};
    SetObjectAttributeAuxSockets aux = {};
    aux.teleport_default.value = test_case.teleport ? 1 : 0;
    InitSetObjectAttributeVectorNode(set_attribute,
                                     aux,
                                     flow,
                                     value,
                                     "Set Object Attribute",
                                     2,
                                     test_case.attribute_type,
                                     &value_default);
    Append(tree.nodes, set_attribute);

    blender::bNodeLink event_to_set = {};
    InitLink(event_to_set, on_update, on_update_pulse, set_attribute, flow);
    Append(tree.links, event_to_set);

    const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
    std::string issues;
    for (const LN_CompileIssue &issue : program->GetCompileReport().GetIssues()) {
      issues += issue.message + "\n";
    }
    ASSERT_FALSE(program->GetCompileReport().HasErrors()) << test_case.attribute_type << "\n"
                                                          << issues;
    const std::vector<LN_Instruction> &instructions = program->GetInstructions(
        LN_Event::OnFixedUpdate);
    const LN_Instruction *write = FindInstruction(instructions, test_case.write);
    const LN_Instruction *reset = FindInstruction(instructions, LN_OpCode::ResetSmoothing);
    ASSERT_NE(write, nullptr) << test_case.attribute_type;
    if (!test_case.reset_expected) {
      EXPECT_EQ(reset, nullptr) << test_case.attribute_type << " " << test_case.teleport;
      continue;
    }
    ASSERT_NE(reset, nullptr) << test_case.attribute_type;
    EXPECT_GT(reset, write);
    EXPECT_NE(reset->bool_expr_index, LN_INVALID_INDEX);
  }
}

TEST(LN_TreeCompiler, WarnsMissingRequiredFlowOnUnreachableCommand)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueBoolean condition_default = {};
  condition_default.value = 1;
  blender::bNode branch = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket condition = {};
  blender::bNodeSocket true_output = {};
  blender::bNodeSocket false_output = {};
  InitNode(branch, "LogicNativeBranch", "Branch", 2);
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(condition, "Condition", blender::SOCK_BOOLEAN, &condition_default);
  InitSocket(true_output, "True", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(false_output, "False", blender::SOCK_BOOLEAN, nullptr);
  Append(branch.inputs, flow);
  Append(branch.inputs, condition);
  Append(branch.outputs, true_output);
  Append(branch.outputs, false_output);
  Append(tree.nodes, branch);

  blender::bNodeLink event_to_branch = {};
  InitLink(event_to_branch, on_update, on_update_pulse, branch, condition);
  Append(tree.links, event_to_branch);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  const LN_CompileReport &report = program->GetCompileReport();
  EXPECT_FALSE(report.HasErrors());

  bool found_flow_condition_warning = false;
  for (const LN_CompileIssue &issue : program->GetCompileReport().GetIssues()) {
    if (issue.severity == LN_CompileSeverity::Warning &&
        issue.message.find("connect execution to 'Flow' and boolean data to 'Condition'") !=
            std::string::npos)
    {
      found_flow_condition_warning = true;
    }
  }
  EXPECT_TRUE(found_flow_condition_warning);
  EXPECT_EQ(FindInstruction(program->GetInstructions(LN_Event::OnUpdate), LN_OpCode::BranchRoute),
            nullptr);
}

TEST(LN_TreeCompiler, RejectsMissingInputSocket)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueFloat a_default = {};
  a_default.value = 1.0f;
  blender::bNode math = {};
  blender::bNodeSocket math_a = {};
  blender::bNodeSocket math_result = {};
  InitNode(math, "LogicNativeMath", "Math", 2);
  InitSocket(math_a, "A", blender::SOCK_FLOAT, &a_default);
  InitSocket(math_result, "Result", blender::SOCK_FLOAT, nullptr);
  Append(math.inputs, math_a);
  Append(math.outputs, math_result);
  Append(tree.nodes, math);

  blender::bNodeSocketValueVector position_default = {};
  blender::bNode set_position = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket position = {};
  SetObjectAttributeAuxSockets set_attribute_aux = {};
  InitSetObjectAttributeVectorNode(
      set_position, set_attribute_aux, flow, position, "Set Position", 3, 0, &position_default);
  Append(tree.nodes, set_position);

  blender::bNodeLink math_to_position = {};
  blender::bNodeLink event_to_set_position = {};
  InitLink(math_to_position, math, math_result, set_position, position);
  InitLink(event_to_set_position, on_update, on_update_pulse, set_position, flow);
  Append(tree.links, math_to_position);
  Append(tree.links, event_to_set_position);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_TRUE(program->GetCompileReport().HasErrors());
}

TEST(LN_TreeCompiler, RejectsUnknownNode)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode unknown = {};
  InitNode(unknown, "LogicNativeFuture", "Future", 1);
  Append(tree.nodes, unknown);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_TRUE(program->GetCompileReport().HasErrors());
}

TEST(LN_TreeCompiler, ProgramMetadataAndSourceChecksumDefineCompatibility)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  const std::shared_ptr<LN_Program> first_program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(first_program, nullptr);
  EXPECT_EQ(first_program->GetProgramVersion(), LN_PROGRAM_VERSION);
  EXPECT_EQ(first_program->GetSchemaVersion(), LN_PROGRAM_SCHEMA_VERSION);
  EXPECT_TRUE(first_program->IsCurrentRuntimeCompatible());
  EXPECT_TRUE(first_program->MatchesSource(first_program->GetSourceTreeName(),
                                           first_program->GetSourceTreeLibraryPath(),
                                           first_program->GetSourceChecksum()));

  blender::bNode on_update = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  Append(tree.nodes, on_update);

  const std::shared_ptr<LN_Program> second_program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(second_program, nullptr);
  EXPECT_TRUE(second_program->IsCurrentRuntimeCompatible());
  EXPECT_NE(first_program->GetSourceChecksum(), second_program->GetSourceChecksum());
  EXPECT_FALSE(second_program->CanPreserveRuntimeStateWhenReplacing(*first_program));
  EXPECT_FALSE(first_program->MatchesSource(first_program->GetSourceTreeName(),
                                            first_program->GetSourceTreeLibraryPath(),
                                            second_program->GetSourceChecksum()));
}

TEST(LN_TreeCompiler, RotationSocketDefaultParticipatesInSourceChecksum)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNodeSocketValueRotation rotation_default = {};
  rotation_default.value_euler[0] = 0.25f;
  rotation_default.value_euler[1] = -0.5f;
  rotation_default.value_euler[2] = 1.0f;

  blender::bNode on_update = {};
  blender::bNodeSocket pulse = {};
  blender::bNodeSocket rotation = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(pulse, "Pulse", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(rotation, "Rotation", blender::SOCK_ROTATION, &rotation_default);
  Append(on_update.inputs, rotation);
  Append(on_update.outputs, pulse);
  Append(tree.nodes, on_update);

  const std::shared_ptr<LN_Program> first_program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(first_program, nullptr);
  EXPECT_FALSE(first_program->GetCompileReport().HasErrors());

  rotation_default.value_euler[2] = 2.0f;

  const std::shared_ptr<LN_Program> second_program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(second_program, nullptr);
  EXPECT_FALSE(second_program->GetCompileReport().HasErrors());
  EXPECT_NE(first_program->GetSourceChecksum(), second_program->GetSourceChecksum());
  EXPECT_FALSE(second_program->CanPreserveRuntimeStateWhenReplacing(*first_program));
}

TEST(LN_TreeCompiler, CurveMappingAuthoredStateParticipatesInSourceChecksum)
{
  const std::array<const char *, 2> curve_node_idnames = {
      "LogicNativeFloatCurve", "LogicNativeTweenValue"};

  for (const char *idname : curve_node_idnames) {
    SCOPED_TRACE(idname);

    blender::bNodeTree tree = {};
    InitTree(tree);
    blender::bNode curve_node = {};
    InitNode(curve_node, idname, "Curve", 1);
    Append(tree.nodes, curve_node);

    std::unique_ptr<blender::CurveMapping, decltype(&blender::BKE_curvemapping_free)>
        mapping_owner(blender::BKE_curvemapping_add(1, 0.0f, 0.0f, 1.0f, 1.0f),
                      &blender::BKE_curvemapping_free);
    blender::CurveMapping *mapping = mapping_owner.get();
    ASSERT_NE(mapping, nullptr);
    ASSERT_GE(mapping->cm[0].totpoint, 2);
    ASSERT_NE(mapping->cm[0].curve, nullptr);
    curve_node.storage = mapping;

    blender::BKE_curvemapping_init(mapping);
    const std::string baseline = LN_TreeCompiler::BuildSourceChecksum(tree);

    /* Derived lookup tables, edit counters, and UI selection do not alter evaluation. */
    blender::BKE_curvemapping_init(mapping);
    mapping->changed_timestamp++;
    mapping->cm[0].curve[0].flag = blender::eCurveMapPoint_Flag(
        int(mapping->cm[0].curve[0].flag) ^ int(blender::CUMA_SELECT));
    EXPECT_EQ(LN_TreeCompiler::BuildSourceChecksum(tree), baseline);
    mapping->cm[0].curve[0].flag = blender::eCurveMapPoint_Flag(
        int(mapping->cm[0].curve[0].flag) ^ int(blender::CUMA_SELECT));

    const float original_y = mapping->cm[0].curve[0].y;
    mapping->cm[0].curve[0].y = original_y + 0.125f;
    blender::BKE_curvemapping_changed(mapping, false);
    EXPECT_NE(LN_TreeCompiler::BuildSourceChecksum(tree), baseline);
    mapping->cm[0].curve[0].y = original_y;
    blender::BKE_curvemapping_changed(mapping, false);
    EXPECT_EQ(LN_TreeCompiler::BuildSourceChecksum(tree), baseline);

    mapping->cm[0].curve[0].flag = blender::eCurveMapPoint_Flag(
        int(mapping->cm[0].curve[0].flag) ^ int(blender::CUMA_HANDLE_VECTOR));
    blender::BKE_curvemapping_changed(mapping, false);
    EXPECT_NE(LN_TreeCompiler::BuildSourceChecksum(tree), baseline);
    mapping->cm[0].curve[0].flag = blender::eCurveMapPoint_Flag(
        int(mapping->cm[0].curve[0].flag) ^ int(blender::CUMA_HANDLE_VECTOR));
    blender::BKE_curvemapping_changed(mapping, false);
    EXPECT_EQ(LN_TreeCompiler::BuildSourceChecksum(tree), baseline);

    mapping->flag = blender::eCurveMappingFlags(
        int(mapping->flag) ^ int(blender::CUMA_EXTEND_EXTRAPOLATE));
    EXPECT_NE(LN_TreeCompiler::BuildSourceChecksum(tree), baseline);
    mapping->flag = blender::eCurveMappingFlags(
        int(mapping->flag) ^ int(blender::CUMA_EXTEND_EXTRAPOLATE));
    EXPECT_EQ(LN_TreeCompiler::BuildSourceChecksum(tree), baseline);

    const float original_clip_min_x = mapping->clipr.xmin;
    mapping->clipr.xmin = original_clip_min_x - 1.0f;
    EXPECT_NE(LN_TreeCompiler::BuildSourceChecksum(tree), baseline);
    mapping->clipr.xmin = original_clip_min_x;
    EXPECT_EQ(LN_TreeCompiler::BuildSourceChecksum(tree), baseline);

    curve_node.storage = nullptr;
    EXPECT_NE(LN_TreeCompiler::BuildSourceChecksum(tree), baseline);
  }
}

bool HasWarningContaining(const LN_CompileReport &report, const char *needle)
{
  for (const LN_CompileIssue &issue : report.GetIssues()) {
    if (issue.severity == LN_CompileSeverity::Warning &&
        issue.message.find(needle) != std::string::npos)
    {
      return true;
    }
  }
  return false;
}

bool HasErrorContaining(const LN_CompileReport &report, const char *needle)
{
  for (const LN_CompileIssue &issue : report.GetIssues()) {
    if (issue.severity == LN_CompileSeverity::Error &&
        issue.message.find(needle) != std::string::npos)
    {
      return true;
    }
  }
  return false;
}

const char *SeverityName(const LN_CompileSeverity severity)
{
  switch (severity) {
    case LN_CompileSeverity::Info:
      return "info";
    case LN_CompileSeverity::Warning:
      return "warning";
    case LN_CompileSeverity::Error:
      return "error";
  }
  return "unknown";
}

std::string FormatCompileReport(const LN_CompileReport &report)
{
  std::string result;
  for (const LN_CompileIssue &issue : report.GetIssues()) {
    result += SeverityName(issue.severity);
    result += ": ";
    result += issue.message;
    result += "\n";
  }
  return result;
}

TEST(LN_TreeCompiler, WarnsForStaleConditionExecutionSocketOnUnreachableCommand)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueVector position_default = {};
  blender::bNode set_position = {};
  blender::bNodeSocket stale_flow = {};
  blender::bNodeSocket position = {};
  SetObjectAttributeAuxSockets set_attribute_aux = {};
  InitSetObjectAttributeVectorNode(set_position,
                                   set_attribute_aux,
                                   stale_flow,
                                   position,
                                   "Set Position",
                                   2,
                                   0,
                                   &position_default,
                                   "Condition");
  Append(tree.nodes, set_position);

  blender::bNodeLink event_to_set_position = {};
  InitLink(event_to_set_position, on_update, on_update_pulse, set_position, stale_flow);
  Append(tree.links, event_to_set_position);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  const LN_CompileReport &report = program->GetCompileReport();

  EXPECT_FALSE(report.HasErrors()) << FormatCompileReport(report);
  EXPECT_TRUE(HasWarningContaining(report, "not part of the native node definition"))
      << FormatCompileReport(report);
}

TEST(LN_TreeCompiler, WarnsForConditionSocketIdnameOnUnreachableCommandFlow)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueVector position_default = {};
  blender::bNode set_position = {};
  blender::bNodeSocket stale_flow = {};
  blender::bNodeSocket position = {};
  SetObjectAttributeAuxSockets set_attribute_aux = {};
  InitSetObjectAttributeVectorNode(set_position,
                                   set_attribute_aux,
                                   stale_flow,
                                   position,
                                   "Set Position",
                                   2,
                                   0,
                                   &position_default,
                                   "Flow",
                                   "NodeSocketLogicCondition");
  Append(tree.nodes, set_position);

  blender::bNodeLink event_to_set_position = {};
  InitLink(event_to_set_position, on_update, on_update_pulse, set_position, stale_flow);
  Append(tree.links, event_to_set_position);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  const LN_CompileReport &report = program->GetCompileReport();

  EXPECT_FALSE(report.HasErrors()) << FormatCompileReport(report);
  EXPECT_TRUE(HasWarningContaining(report, "not part of the native node definition"))
      << FormatCompileReport(report);
  EXPECT_EQ(
      FindInstruction(program->GetInstructions(LN_Event::OnUpdate), LN_OpCode::SetWorldPosition),
      nullptr);
}

TEST(LN_TreeCompiler, RejectsHiddenUnsupportedRuntimeNodeEvenWhenInactive)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode hidden_node = {};
  InitNode(hidden_node, "LogicNativeSetNodeGroupSocketValue", "Set Node Group Socket", 1);
  Append(tree.nodes, hidden_node);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  const LN_CompileReport &report = program->GetCompileReport();

  ASSERT_TRUE(report.HasErrors()) << FormatCompileReport(report);
  EXPECT_EQ(report.GetDisabledReason(), "Logic Nodes compilation failed");
  EXPECT_TRUE(HasErrorContaining(report, "runtime command implementation"))
      << FormatCompileReport(report);
}

bool HasColorInstruction(const std::vector<LN_Instruction> &instructions,
                         LN_OpCode opcode,
                         const MT_Vector4 &expected)
{
  for (const LN_Instruction &instruction : instructions) {
    if (instruction.opcode == opcode && (instruction.color_value - expected).length() < 0.0001f) {
      return true;
    }
  }
  return false;
}

TEST(LN_TreeCompiler, IgnoresUnconnectedCommandNodesOnActiveChain)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_init = {};
  blender::bNodeSocket on_init_pulse = {};
  InitNode(on_init, "LogicNativeOnInit", "On Init", 1);
  InitSocket(on_init_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_init.outputs, on_init_pulse);
  Append(tree.nodes, on_init);

  blender::bNodeSocketValueRGBA color_default = {};
  color_default.value[0] = 0.0f;
  color_default.value[1] = 0.0f;
  color_default.value[2] = 1.0f;
  color_default.value[3] = 1.0f;
  blender::bNode set_light_connected = {};
  blender::bNodeSocket set_light_flow = {};
  blender::bNodeSocket set_light_color = {};
  InitNode(set_light_connected, "LogicNativeSetLightColor", "Set Light Connected", 2);
  InitSocket(set_light_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(set_light_color, "Color", blender::SOCK_RGBA, &color_default);
  Append(set_light_connected.inputs, set_light_flow);
  Append(set_light_connected.inputs, set_light_color);
  Append(tree.nodes, set_light_connected);

  blender::bNodeSocketValueRGBA orphan_color_default = {};
  orphan_color_default.value[0] = 1.0f;
  orphan_color_default.value[1] = 0.0f;
  orphan_color_default.value[2] = 0.0f;
  orphan_color_default.value[3] = 1.0f;
  blender::bNode set_light_orphan = {};
  blender::bNodeSocket orphan_flow = {};
  blender::bNodeSocket orphan_color = {};
  InitNode(set_light_orphan, "LogicNativeSetLightColor", "Set Light Orphan", 3);
  InitSocket(orphan_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(orphan_color, "Color", blender::SOCK_RGBA, &orphan_color_default);
  Append(set_light_orphan.inputs, orphan_flow);
  Append(set_light_orphan.inputs, orphan_color);
  Append(tree.nodes, set_light_orphan);

  blender::bNodeLink flow_link = {};
  InitLink(flow_link, on_init, on_init_pulse, set_light_connected, set_light_flow);
  Append(tree.links, flow_link);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors());
  EXPECT_TRUE(
      HasWarningContaining(program->GetCompileReport(), "not connected to an event chain"));
  EXPECT_TRUE(HasColorInstruction(program->GetInstructions(LN_Event::OnInit),
                                  LN_OpCode::SetLightColor,
                                  MT_Vector4(0.0f, 0.0f, 1.0f, 1.0f)));

  int set_light_count = 0;
  for (const LN_Instruction &instruction : program->GetInstructions(LN_Event::OnInit)) {
    if (instruction.opcode == LN_OpCode::SetLightColor) {
      set_light_count++;
    }
  }
  EXPECT_EQ(set_light_count, 1);
}

TEST(LN_TreeCompiler, EmitsSetWorldPositionFromOnInit)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_init = {};
  blender::bNodeSocket on_init_pulse = {};
  InitNode(on_init, "LogicNativeOnInit", "On Init", 1);
  InitSocket(on_init_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_init.outputs, on_init_pulse);
  Append(tree.nodes, on_init);

  blender::bNodeSocketValueVector position_source_default = {};
  position_source_default.value[0] = 1.0f;
  position_source_default.value[1] = 2.0f;
  position_source_default.value[2] = 3.0f;
  blender::bNode position_source = {};
  blender::bNodeSocket position_source_output = {};
  InitNode(position_source, "LogicNativeValueVector", "Position", 2);
  InitSocket(position_source_output, "Vector", blender::SOCK_VECTOR, &position_source_default);
  Append(position_source.outputs, position_source_output);
  Append(tree.nodes, position_source);

  blender::bNodeSocketValueVector position_default = {};
  blender::bNode set_position = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket position = {};
  SetObjectAttributeAuxSockets set_attribute_aux = {};
  InitSetObjectAttributeVectorNode(
      set_position, set_attribute_aux, flow, position, "Set Position", 3, 0, &position_default);
  Append(tree.nodes, set_position);

  blender::bNodeLink flow_link = {};
  blender::bNodeLink position_link = {};
  InitLink(flow_link, on_init, on_init_pulse, set_position, flow);
  InitLink(position_link, position_source, position_source_output, set_position, position);
  Append(tree.links, flow_link);
  Append(tree.links, position_link);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors());
  EXPECT_TRUE(HasVectorInstruction(program->GetInstructions(LN_Event::OnInit),
                                   LN_OpCode::SetTransformVector,
                                   MT_Vector3(1.0f, 2.0f, 3.0f)));
  const LN_Instruction *instruction = FindInstruction(program->GetInstructions(LN_Event::OnInit),
                                                      LN_OpCode::SetTransformVector);
  ASSERT_NE(instruction, nullptr);
  EXPECT_EQ(instruction->vector_operation_mode, uint8_t(LN_VectorOperationMode::World));
  EXPECT_EQ(instruction->vector_operation_channel, uint8_t(LN_VectorOperationChannel::Position));
  EXPECT_EQ(instruction->vector_operation_mask, LN_VECTOR_OPERATION_MASK_ALL);
}

TEST(LN_TreeCompiler, EmitsCommandsChainedFromPrintDoneOutput)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnFixedUpdate", "On Fixed Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueString message_default = {};
  SetCString(message_default.value, "hello");
  blender::bNode print_node = {};
  blender::bNodeSocket print_flow = {};
  blender::bNodeSocket print_message = {};
  blender::bNodeSocket print_done = {};
  InitNode(print_node, "LogicNativePrint", "Print", 2);
  InitSocket(print_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(print_message, "Message", blender::SOCK_STRING, &message_default);
  InitSocket(print_done, "Done", blender::SOCK_BOOLEAN, nullptr);
  Append(print_node.inputs, print_flow);
  Append(print_node.inputs, print_message);
  Append(print_node.outputs, print_done);
  Append(tree.nodes, print_node);

  blender::bNodeSocketValueVector position_default = {};
  position_default.value[0] = 3.0f;
  position_default.value[1] = 4.0f;
  position_default.value[2] = 5.0f;
  blender::bNode set_position = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket position = {};
  SetObjectAttributeAuxSockets set_attribute_aux = {};
  InitSetObjectAttributeVectorNode(
      set_position, set_attribute_aux, flow, position, "Set Position", 3, 0, &position_default);
  Append(tree.nodes, set_position);

  blender::bNodeLink event_to_print = {};
  blender::bNodeLink print_done_to_position = {};
  InitLink(event_to_print, on_update, on_update_pulse, print_node, print_flow);
  InitLink(print_done_to_position, print_node, print_done, set_position, flow);
  Append(tree.links, event_to_print);
  Append(tree.links, print_done_to_position);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors());
  EXPECT_NE(nullptr,
            FindInstruction(program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::Print));
  EXPECT_TRUE(HasVectorInstruction(program->GetInstructions(LN_Event::OnFixedUpdate),
                                   LN_OpCode::SetTransformVector,
                                   MT_Vector3(3.0f, 4.0f, 5.0f)));
}

TEST(LN_TreeCompiler, GetParentDefaultsChildToLogicTreeOwner)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNode get_parent = {};
  blender::bNodeSocket child_object = {};
  blender::bNodeSocket parent_object = {};
  blender::bNodeSocketValueObject child_object_default = {};
  InitNode(get_parent, "LogicNativeGetParent", "Get Parent", 2);
  InitSocket(child_object, "Child Object", blender::SOCK_OBJECT, &child_object_default);
  InitSocket(parent_object, "Parent Object", blender::SOCK_OBJECT, nullptr);
  Append(get_parent.inputs, child_object);
  Append(get_parent.outputs, parent_object);
  Append(tree.nodes, get_parent);

  blender::bNode print_node = {};
  blender::bNodeSocket print_flow = {};
  blender::bNodeSocket print_value = {};
  InitNode(print_node, "LogicNativePrint", "Print", 3);
  InitSocket(print_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(print_value, "Value", blender::SOCK_CUSTOM, nullptr);
  Append(print_node.inputs, print_flow);
  Append(print_node.inputs, print_value);
  Append(tree.nodes, print_node);

  blender::bNodeLink event_to_print = {};
  blender::bNodeLink parent_to_print = {};
  InitLink(event_to_print, on_update, on_update_pulse, print_node, print_flow);
  InitLink(parent_to_print, get_parent, parent_object, print_node, print_value);
  Append(tree.links, event_to_print);
  Append(tree.links, parent_to_print);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors());

  const LN_Instruction *print_instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnUpdate), LN_OpCode::Print);
  ASSERT_NE(print_instruction, nullptr);
  const LN_ValueExpression *parent_expression = ValueExpressionAt(
      *program, print_instruction->value_expr_index);
  ASSERT_NE(parent_expression, nullptr);
  EXPECT_EQ(parent_expression->kind, LN_ValueExpressionKind::ObjectParent);

  const LN_ValueExpression *owner_expression = ValueExpressionAt(*program,
                                                                 parent_expression->input0);
  ASSERT_NE(owner_expression, nullptr);
  EXPECT_EQ(owner_expression->kind, LN_ValueExpressionKind::OwnerObject);
}

TEST(LN_TreeCompiler, SaveAndLoadDoneRoutesOnlyAfterPersistenceCompletion)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_init = {};
  blender::bNodeSocket init_out = {};
  InitNode(on_init, "LogicNativeOnInit", "On Init", 1);
  InitSocket(init_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_init.outputs, init_out);
  Append(tree.nodes, on_init);

  blender::bNode on_update = {};
  blender::bNodeSocket update_out = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 2);
  InitSocket(update_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, update_out);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueInt save_slot_default = {};
  save_slot_default.value = 1;
  blender::bNodeSocketValueString save_path_default = {};
  SetCString(save_path_default.value, "//checkpoints");
  blender::bNode save = {};
  blender::bNodeSocket save_flow = {};
  blender::bNodeSocket save_slot = {};
  blender::bNodeSocket save_path = {};
  blender::bNodeSocket save_done = {};
  blender::bNodeSocket save_success = {};
  InitNode(save, "LogicNativeSaveGame", "Save Game", 3);
  InitSocket(save_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(save_slot, "Slot", blender::SOCK_INT, &save_slot_default);
  InitSocket(save_path, "Path", blender::SOCK_STRING, &save_path_default);
  InitSocket(save_done, "Done", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(save_success, "Success", blender::SOCK_BOOLEAN, nullptr);
  Append(save.inputs, save_flow);
  Append(save.inputs, save_slot);
  Append(save.inputs, save_path);
  Append(save.outputs, save_done);
  Append(save.outputs, save_success);
  Append(tree.nodes, save);

  blender::bNodeSocketValueInt load_slot_default = {};
  load_slot_default.value = 1;
  blender::bNodeSocketValueString load_path_default = {};
  SetCString(load_path_default.value, "//checkpoints");
  blender::bNode load = {};
  blender::bNodeSocket load_flow = {};
  blender::bNodeSocket load_slot = {};
  blender::bNodeSocket load_path = {};
  blender::bNodeSocket load_done = {};
  blender::bNodeSocket load_success = {};
  InitNode(load, "LogicNativeLoadGame", "Load Game", 4);
  InitSocket(load_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(load_slot, "Slot", blender::SOCK_INT, &load_slot_default);
  InitSocket(load_path, "Path", blender::SOCK_STRING, &load_path_default);
  InitSocket(load_done, "Done", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(load_success, "Success", blender::SOCK_BOOLEAN, nullptr);
  Append(load.inputs, load_flow);
  Append(load.inputs, load_slot);
  Append(load.inputs, load_path);
  Append(load.outputs, load_done);
  Append(load.outputs, load_success);
  Append(tree.nodes, load);

  blender::bNode quit = {};
  blender::bNodeSocket quit_flow = {};
  InitNode(quit, "LogicNativeQuitGame", "Quit", 5);
  InitSocket(quit_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  Append(quit.inputs, quit_flow);
  Append(tree.nodes, quit);

  blender::bNodeSocketValueString message_default = {};
  SetCString(message_default.value, "loaded");
  blender::bNode print_node = {};
  blender::bNodeSocket print_flow = {};
  blender::bNodeSocket print_message = {};
  InitNode(print_node, "LogicNativePrint", "Print", 6);
  InitSocket(print_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(print_message, "Message", blender::SOCK_STRING, &message_default);
  Append(print_node.inputs, print_flow);
  Append(print_node.inputs, print_message);
  Append(tree.nodes, print_node);

  blender::bNodeLink init_to_save = {};
  blender::bNodeLink save_to_quit = {};
  blender::bNodeLink update_to_load = {};
  blender::bNodeLink load_to_print = {};
  InitLink(init_to_save, on_init, init_out, save, save_flow);
  InitLink(save_to_quit, save, save_done, quit, quit_flow);
  InitLink(update_to_load, on_update, update_out, load, load_flow);
  InitLink(load_to_print, load, load_done, print_node, print_flow);
  Append(tree.links, init_to_save);
  Append(tree.links, save_to_quit);
  Append(tree.links, update_to_load);
  Append(tree.links, load_to_print);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  const LN_Instruction *save_instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnInit), LN_OpCode::SaveGame);
  const LN_Instruction *load_instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnUpdate), LN_OpCode::LoadGame);
  ASSERT_NE(save_instruction, nullptr);
  ASSERT_NE(load_instruction, nullptr);
  ASSERT_NE(BoolExpressionAt(*program, save_instruction->bool_expr_index), nullptr);
  ASSERT_NE(BoolExpressionAt(*program, load_instruction->bool_expr_index), nullptr);
  EXPECT_EQ(BoolExpressionAt(*program, save_instruction->bool_expr_index)->kind,
            LN_BoolExpressionKind::PersistenceCompleted);
  EXPECT_EQ(BoolExpressionAt(*program, load_instruction->bool_expr_index)->kind,
            LN_BoolExpressionKind::PersistenceCompleted);

  size_t success_expression_count = 0;
  for (const LN_BoolExpression &expression : program->GetBoolExpressions()) {
    if (expression.kind == LN_BoolExpressionKind::PersistenceSucceeded) {
      success_expression_count++;
      EXPECT_TRUE(expression.input0 == save_instruction->bool_expr_index ||
                  expression.input0 == load_instruction->bool_expr_index);
    }
  }
  EXPECT_EQ(success_expression_count, 2u);

  EXPECT_EQ(FindInstruction(program->GetInstructions(LN_Event::OnInit), LN_OpCode::QuitGame),
            nullptr);
  const LN_Instruction *completed_save = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::QuitGame);
  const LN_Instruction *completed_load = FindInstruction(
      program->GetInstructions(LN_Event::OnUpdate), LN_OpCode::Print);
  ASSERT_NE(completed_save, nullptr);
  ASSERT_NE(completed_load, nullptr);
  EXPECT_TRUE(BoolExpressionTreeContainsKind(*program,
                                             completed_save->bool_guard_expr_index,
                                             LN_BoolExpressionKind::PersistenceCompleted));
  EXPECT_TRUE(BoolExpressionTreeContainsKind(*program,
                                             completed_load->bool_guard_expr_index,
                                             LN_BoolExpressionKind::PersistenceCompleted));
}

TEST(LN_TreeCompiler, SetSaveExistenceUsesOwnerDefaultAndTerminalCompletion)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket update_out = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(update_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, update_out);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueBoolean enabled_default = {};
  enabled_default.value = 0;
  blender::bNode set_existence = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket object = {};
  blender::bNodeSocket enabled = {};
  blender::bNodeSocket done = {};
  blender::bNodeSocket success = {};
  InitNode(set_existence, "LogicNativeSetSaveExistence", "Set Save Object State", 2);
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(enabled, "Enabled", blender::SOCK_BOOLEAN, &enabled_default);
  InitSocket(done, "Done", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(success, "Success", blender::SOCK_BOOLEAN, nullptr);
  Append(set_existence.inputs, flow);
  Append(set_existence.inputs, object);
  Append(set_existence.inputs, enabled);
  Append(set_existence.outputs, done);
  Append(set_existence.outputs, success);
  Append(tree.nodes, set_existence);

  blender::bNode quit = {};
  blender::bNodeSocket quit_flow = {};
  InitNode(quit, "LogicNativeQuitGame", "Quit", 3);
  InitSocket(quit_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  Append(quit.inputs, quit_flow);
  Append(tree.nodes, quit);

  blender::bNodeLink update_to_set = {};
  blender::bNodeLink done_to_quit = {};
  InitLink(update_to_set, on_update, update_out, set_existence, flow);
  InitLink(done_to_quit, set_existence, done, quit, quit_flow);
  Append(tree.links, update_to_set);
  Append(tree.links, done_to_quit);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  const LN_Instruction *instruction = FindInstruction(program->GetInstructions(LN_Event::OnUpdate),
                                                      LN_OpCode::SetSaveExistence);
  ASSERT_NE(instruction, nullptr);
  EXPECT_EQ(instruction->value_expr_index, LN_INVALID_INDEX);
  const LN_BoolExpression *enabled_expression = BoolExpressionAt(*program,
                                                                 instruction->bool_expr_index);
  const LN_BoolExpression *completion_expression = BoolExpressionAt(
      *program, instruction->secondary_bool_expr_index);
  ASSERT_NE(enabled_expression, nullptr);
  ASSERT_NE(completion_expression, nullptr);
  EXPECT_EQ(enabled_expression->kind, LN_BoolExpressionKind::Constant);
  EXPECT_FALSE(enabled_expression->bool_value);
  EXPECT_EQ(completion_expression->kind, LN_BoolExpressionKind::PersistenceCompleted);

  bool found_success = false;
  for (const LN_BoolExpression &expression : program->GetBoolExpressions()) {
    found_success |= expression.kind == LN_BoolExpressionKind::PersistenceSucceeded &&
                     expression.input0 == instruction->secondary_bool_expr_index;
  }
  EXPECT_TRUE(found_success);
  const LN_Instruction *completed = FindInstruction(program->GetInstructions(LN_Event::OnUpdate),
                                                    LN_OpCode::QuitGame);
  ASSERT_NE(completed, nullptr);
  EXPECT_TRUE(BoolExpressionTreeContainsKind(
      *program, completed->bool_guard_expr_index, LN_BoolExpressionKind::PersistenceCompleted));
}

TEST(LN_TreeCompiler, PrintAcceptsGenericValueInput)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueFloat value_default = {};
  value_default.value = 12.5f;
  blender::bNode value = {};
  blender::bNodeSocket value_output = {};
  InitNode(value, "LogicNativeValueFloat", "Value", 2);
  InitSocket(value_output, "Float", blender::SOCK_FLOAT, &value_default);
  Append(value.outputs, value_output);
  Append(tree.nodes, value);

  blender::bNode print_node = {};
  blender::bNodeSocket print_flow = {};
  blender::bNodeSocket print_message = {};
  InitNode(print_node, "LogicNativePrint", "Print", 3);
  InitSocket(print_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(print_message, "Message", blender::SOCK_CUSTOM, nullptr);
  Append(print_node.inputs, print_flow);
  Append(print_node.inputs, print_message);
  Append(tree.nodes, print_node);

  blender::bNodeLink event_to_print = {};
  blender::bNodeLink value_to_print = {};
  InitLink(event_to_print, on_update, on_update_pulse, print_node, print_flow);
  InitLink(value_to_print, value, value_output, print_node, print_message);
  Append(tree.links, event_to_print);
  Append(tree.links, value_to_print);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors());

  const LN_Instruction *print_instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnUpdate), LN_OpCode::Print);
  ASSERT_NE(print_instruction, nullptr);
  ASSERT_LT(print_instruction->value_expr_index, program->GetValueExpressions().size());
  EXPECT_EQ(program->GetValueExpressions()[print_instruction->value_expr_index].kind,
            LN_ValueExpressionKind::FromFloat);
}

TEST(LN_TreeCompiler, MixValueCompilesEverySocketToNativeRegisterExpressions)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket update_out = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(update_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, update_out);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueFloat a_default = {};
  blender::bNodeSocketValueFloat b_default = {};
  a_default.value = 10.0f;
  b_default.value = 30.0f;
  blender::bNode a_node = {};
  blender::bNode b_node = {};
  blender::bNodeSocket a_out = {};
  blender::bNodeSocket b_out = {};
  InitNode(a_node, "LogicNativeValueFloat", "A", 2);
  InitNode(b_node, "LogicNativeValueFloat", "B", 3);
  InitSocket(a_out, "Float", blender::SOCK_FLOAT, &a_default);
  InitSocket(b_out, "Float", blender::SOCK_FLOAT, &b_default);
  Append(a_node.outputs, a_out);
  Append(b_node.outputs, b_out);
  Append(tree.nodes, a_node);
  Append(tree.nodes, b_node);

  blender::bNodeSocketValueFloat factor_default = {};
  factor_default.value = 0.25f;
  blender::bNode mix = {};
  blender::bNodeSocket mix_a = {};
  blender::bNodeSocket mix_b = {};
  blender::bNodeSocket mix_factor = {};
  blender::bNodeSocket mix_result = {};
  InitNode(mix, "LogicNativeMixValue", "Mix Value", 4);
  InitSocket(mix_a, "A", blender::SOCK_CUSTOM, nullptr, "NodeSocketLogicGeneric");
  InitSocket(mix_b, "B", blender::SOCK_CUSTOM, nullptr, "NodeSocketLogicGeneric");
  InitSocket(mix_factor, "Factor", blender::SOCK_FLOAT, &factor_default);
  InitSocket(mix_result, "Result", blender::SOCK_CUSTOM, nullptr, "NodeSocketLogicGeneric");
  Append(mix.inputs, mix_a);
  Append(mix.inputs, mix_b);
  Append(mix.inputs, mix_factor);
  Append(mix.outputs, mix_result);
  Append(tree.nodes, mix);

  blender::bNode print_node = {};
  blender::bNodeSocket print_flow = {};
  blender::bNodeSocket print_message = {};
  InitNode(print_node, "LogicNativePrint", "Print", 5);
  InitSocket(print_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(print_message, "Message", blender::SOCK_CUSTOM, nullptr, "NodeSocketLogicGeneric");
  Append(print_node.inputs, print_flow);
  Append(print_node.inputs, print_message);
  Append(tree.nodes, print_node);

  std::array<blender::bNodeLink, 4> links = {};
  InitLink(links[0], a_node, a_out, mix, mix_a);
  InitLink(links[1], b_node, b_out, mix, mix_b);
  InitLink(links[2], on_update, update_out, print_node, print_flow);
  InitLink(links[3], mix, mix_result, print_node, print_message);
  for (blender::bNodeLink &link : links) {
    Append(tree.links, link);
  }

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  const LN_ValueExpression *mix_expression = nullptr;
  for (const LN_ValueExpression &expression : program->GetValueExpressions()) {
    if (expression.kind == LN_ValueExpressionKind::Mix) {
      mix_expression = &expression;
      break;
    }
  }
  ASSERT_NE(mix_expression, nullptr);
  const LN_ValueExpression *compiled_a = ValueExpressionAt(*program, mix_expression->input0);
  const LN_ValueExpression *compiled_b = ValueExpressionAt(*program, mix_expression->input1);
  const LN_FloatExpression *compiled_factor = FloatExpressionAt(*program, mix_expression->input2);
  ASSERT_NE(compiled_a, nullptr);
  ASSERT_NE(compiled_b, nullptr);
  ASSERT_NE(compiled_factor, nullptr);
  EXPECT_EQ(compiled_a->kind, LN_ValueExpressionKind::FromFloat);
  EXPECT_EQ(compiled_b->kind, LN_ValueExpressionKind::FromFloat);
  EXPECT_EQ(compiled_factor->kind, LN_FloatExpressionKind::Constant);
  EXPECT_FLOAT_EQ(compiled_factor->float_value, 0.25f);

  const LN_RegisterExpressionProgram &ir = program->GetRegisterExpressionProgram();
  EXPECT_TRUE(ir.valid);
  EXPECT_EQ(ir.fallback_expression_count, 0u);
  EXPECT_TRUE(std::any_of(ir.ops.begin(), ir.ops.end(), [](const LN_RegisterExpressionOp &op) {
    return op.kind == LN_RegisterExpressionOpKind::ValueMix;
  }));
}

TEST(LN_TreeCompiler, MapRangeUsesBlenderSafeDivideInDynamicAndRegisterPaths)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket update_out = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(update_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, update_out);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueString property_name_default = {};
  SetCString(property_name_default.value, "map_source");
  blender::bNode get_property = {};
  blender::bNodeSocket property_object = {};
  blender::bNodeSocket property_name = {};
  blender::bNodeSocket property_value = {};
  InitNode(get_property, "LogicNativeGetGamePropertyFloat", "Get Map Source", 2);
  InitSocket(property_object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(property_name, "Property", blender::SOCK_STRING, &property_name_default);
  InitSocket(property_value, "Value", blender::SOCK_FLOAT, nullptr);
  Append(get_property.inputs, property_object);
  Append(get_property.inputs, property_name);
  Append(get_property.outputs, property_value);
  Append(tree.nodes, get_property);

  blender::bNodeSocketValueFloat value_default = {};
  blender::bNodeSocketValueFloat from_min_default = {};
  blender::bNodeSocketValueFloat from_max_default = {};
  blender::bNodeSocketValueFloat to_min_default = {};
  blender::bNodeSocketValueFloat to_max_default = {};
  from_max_default.value = 1.0e-25f;
  to_min_default.value = 10.0f;
  to_max_default.value = 20.0f;
  blender::bNode map_range = {};
  blender::bNodeSocket value = {};
  blender::bNodeSocket from_min = {};
  blender::bNodeSocket from_max = {};
  blender::bNodeSocket to_min = {};
  blender::bNodeSocket to_max = {};
  blender::bNodeSocket result = {};
  InitNode(map_range, "LogicNativeMapRange", "Map Range", 3);
  map_range.custom1 = 1;
  InitSocket(value, "Value", blender::SOCK_FLOAT, &value_default);
  InitSocket(from_min, "From Min", blender::SOCK_FLOAT, &from_min_default);
  InitSocket(from_max, "From Max", blender::SOCK_FLOAT, &from_max_default);
  InitSocket(to_min, "To Min", blender::SOCK_FLOAT, &to_min_default);
  InitSocket(to_max, "To Max", blender::SOCK_FLOAT, &to_max_default);
  InitSocket(result, "Result", blender::SOCK_FLOAT, nullptr);
  Append(map_range.inputs, value);
  Append(map_range.inputs, from_min);
  Append(map_range.inputs, from_max);
  Append(map_range.inputs, to_min);
  Append(map_range.inputs, to_max);
  Append(map_range.outputs, result);
  Append(tree.nodes, map_range);

  blender::bNode print_node = {};
  blender::bNodeSocket print_flow = {};
  blender::bNodeSocket print_message = {};
  InitNode(print_node, "LogicNativePrint", "Print", 4);
  InitSocket(print_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(print_message, "Message", blender::SOCK_CUSTOM, nullptr, "NodeSocketLogicGeneric");
  Append(print_node.inputs, print_flow);
  Append(print_node.inputs, print_message);
  Append(tree.nodes, print_node);

  std::array<blender::bNodeLink, 3> links = {};
  InitLink(links[0], get_property, property_value, map_range, value);
  InitLink(links[1], on_update, update_out, print_node, print_flow);
  InitLink(links[2], map_range, result, print_node, print_message);
  for (blender::bNodeLink &link : links) {
    Append(tree.links, link);
  }

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  EXPECT_TRUE(std::any_of(program->GetFloatExpressions().begin(),
                          program->GetFloatExpressions().end(),
                          [](const LN_FloatExpression &expression) {
                            return expression.kind == LN_FloatExpressionKind::SafeDivide;
                          }));

  const LN_RegisterExpressionProgram &ir = program->GetRegisterExpressionProgram();
  EXPECT_TRUE(ir.valid);
  EXPECT_EQ(ir.fallback_expression_count, 0u);
  EXPECT_TRUE(std::any_of(ir.ops.begin(), ir.ops.end(), [](const LN_RegisterExpressionOp &op) {
    return op.kind == LN_RegisterExpressionOpKind::FloatSafeDivide;
  }));
}

TEST(LN_TreeCompiler, FloatCurveBakesMappingAndCompilesEverySocketToNativeRegisters)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket update_out = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(update_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, update_out);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueFloat factor_source_default = {};
  blender::bNodeSocketValueFloat value_source_default = {};
  factor_source_default.value = 0.5f;
  value_source_default.value = 0.25f;
  blender::bNode factor_source = {};
  blender::bNode value_source = {};
  blender::bNodeSocket factor_source_output = {};
  blender::bNodeSocket value_source_output = {};
  InitNode(factor_source, "LogicNativeValueFloat", "Factor", 2);
  InitNode(value_source, "LogicNativeValueFloat", "Value", 3);
  InitSocket(factor_source_output, "Float", blender::SOCK_FLOAT, &factor_source_default);
  InitSocket(value_source_output, "Float", blender::SOCK_FLOAT, &value_source_default);
  Append(factor_source.outputs, factor_source_output);
  Append(value_source.outputs, value_source_output);
  Append(tree.nodes, factor_source);
  Append(tree.nodes, value_source);

  blender::bNodeSocketValueFloat factor_default = {};
  blender::bNodeSocketValueFloat value_default = {};
  blender::bNode curve = {};
  blender::bNodeSocket curve_factor = {};
  blender::bNodeSocket curve_value = {};
  blender::bNodeSocket curve_result = {};
  InitNode(curve, "LogicNativeFloatCurve", "Float Curve", 4);
  InitSocket(curve_factor, "Factor", blender::SOCK_FLOAT, &factor_default);
  InitSocket(curve_value, "Value", blender::SOCK_FLOAT, &value_default);
  InitSocket(curve_result, "Value", blender::SOCK_FLOAT, nullptr);
  Append(curve.inputs, curve_factor);
  Append(curve.inputs, curve_value);
  Append(curve.outputs, curve_result);
  std::unique_ptr<blender::CurveMapping, decltype(&blender::BKE_curvemapping_free)> mapping_owner(
      blender::BKE_curvemapping_add(1, 0.0f, 0.0f, 1.0f, 1.0f),
      &blender::BKE_curvemapping_free);
  curve.storage = mapping_owner.get();
  auto *mapping = mapping_owner.get();
  ASSERT_NE(mapping, nullptr);
  ASSERT_GE(mapping->cm[0].totpoint, 2);
  mapping->cm[0].curve[0].y = 0.0f;
  mapping->cm[0].curve[1].y = 1.0f;
  ASSERT_NE(blender::BKE_curvemap_insert(&mapping->cm[0], 0.4f, 0.85f), nullptr);
  blender::BKE_curvemapping_changed(mapping, false);
  Append(tree.nodes, curve);

  blender::bNode print_node = {};
  blender::bNodeSocket print_flow = {};
  blender::bNodeSocket print_message = {};
  InitNode(print_node, "LogicNativePrint", "Print", 5);
  InitSocket(print_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(print_message, "Message", blender::SOCK_CUSTOM, nullptr, "NodeSocketLogicGeneric");
  Append(print_node.inputs, print_flow);
  Append(print_node.inputs, print_message);
  Append(tree.nodes, print_node);

  std::array<blender::bNodeLink, 4> links = {};
  InitLink(links[0], factor_source, factor_source_output, curve, curve_factor);
  InitLink(links[1], value_source, value_source_output, curve, curve_value);
  InitLink(links[2], on_update, update_out, print_node, print_flow);
  InitLink(links[3], curve, curve_result, print_node, print_message);
  for (blender::bNodeLink &link : links) {
    Append(tree.links, link);
  }

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  const LN_FloatExpression *curve_expression = nullptr;
  for (const LN_FloatExpression &expression : program->GetFloatExpressions()) {
    if (expression.kind == LN_FloatExpressionKind::FloatCurve) {
      curve_expression = &expression;
      break;
    }
  }
  ASSERT_NE(curve_expression, nullptr);
  const LN_FloatExpression *compiled_value = FloatExpressionAt(*program,
                                                               curve_expression->input0);
  const LN_FloatExpression *compiled_factor = FloatExpressionAt(*program,
                                                                curve_expression->input1);
  ASSERT_NE(compiled_value, nullptr);
  ASSERT_NE(compiled_factor, nullptr);
  EXPECT_EQ(compiled_value->kind, LN_FloatExpressionKind::Constant);
  EXPECT_EQ(compiled_factor->kind, LN_FloatExpressionKind::Constant);
  EXPECT_FLOAT_EQ(compiled_value->float_value, 0.25f);
  EXPECT_FLOAT_EQ(compiled_factor->float_value, 0.5f);

  ASSERT_GE(curve_expression->int_value, 0);
  ASSERT_LT(size_t(curve_expression->int_value), program->GetCurveTables().size());
  const LN_CurveTable &table = program->GetCurveTables()[size_t(curve_expression->int_value)];
  ASSERT_NE(mapping->cm[0].table, nullptr);
  EXPECT_FLOAT_EQ(table.domain_min, 0.0f);
  EXPECT_FLOAT_EQ(table.domain_max, 1.0f);
  EXPECT_FLOAT_EQ(table.sample_scale, mapping->cm[0].range);
  EXPECT_FLOAT_EQ(table.table_min_x, mapping->cm[0].table[0].x);
  EXPECT_FLOAT_EQ(table.table_max_x,
                  mapping->cm[0].table[LN_CURVE_SAMPLE_COUNT - 1].x);
  EXPECT_NEAR(table.samples.front(), 0.0f, 1.0e-6f);
  EXPECT_NEAR(table.samples.back(), 1.0f, 1.0e-6f);
  for (int sample_index = 0; sample_index < LN_CURVE_SAMPLE_COUNT; sample_index++) {
    EXPECT_FLOAT_EQ(table.samples[size_t(sample_index)], mapping->cm[0].table[sample_index].y);
  }
  EXPECT_FLOAT_EQ(table.extend_in_x, mapping->cm[0].ext_in[0]);
  EXPECT_FLOAT_EQ(table.extend_in_y, mapping->cm[0].ext_in[1]);
  EXPECT_FLOAT_EQ(table.extend_out_x, mapping->cm[0].ext_out[0]);
  EXPECT_FLOAT_EQ(table.extend_out_y, mapping->cm[0].ext_out[1]);
  EXPECT_EQ(table.extrapolate,
            (mapping->flag & blender::CUMA_EXTEND_EXTRAPOLATE) != 0);
  EXPECT_EQ(table.clip_output, (mapping->flag & blender::CUMA_DO_CLIP) != 0);
  EXPECT_FLOAT_EQ(table.clip_min, mapping->clipr.ymin);
  EXPECT_FLOAT_EQ(table.clip_max, mapping->clipr.ymax);

  const LN_RegisterExpressionProgram &ir = program->GetRegisterExpressionProgram();
  EXPECT_TRUE(ir.valid);
  EXPECT_EQ(ir.fallback_expression_count, 0u);
  EXPECT_TRUE(std::any_of(ir.ops.begin(), ir.ops.end(), [](const LN_RegisterExpressionOp &op) {
    return op.kind == LN_RegisterExpressionOpKind::FloatCurve;
  }));
  curve.storage = nullptr;
}

TEST(LN_TreeCompiler, MixVectorFeedsPositionAndRotationTypedConsumers)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket update_out = {};
  InitNode(on_update, "LogicNativeOnFixedUpdate", "On Fixed Update", 1);
  InitSocket(update_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, update_out);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueVector a_default = {};
  blender::bNodeSocketValueVector b_default = {};
  a_default.value[2] = -10.0f;
  b_default.value[2] = 5.0f;
  blender::bNode a_node = {};
  blender::bNode b_node = {};
  blender::bNodeSocket a_out = {};
  blender::bNodeSocket b_out = {};
  InitNode(a_node, "LogicNativeValueVector", "A", 2);
  InitNode(b_node, "LogicNativeValueVector", "B", 3);
  InitSocket(a_out, "Vector", blender::SOCK_VECTOR, &a_default);
  InitSocket(b_out, "Vector", blender::SOCK_VECTOR, &b_default);
  Append(a_node.outputs, a_out);
  Append(b_node.outputs, b_out);
  Append(tree.nodes, a_node);
  Append(tree.nodes, b_node);

  blender::bNodeSocketValueFloat factor_default = {};
  factor_default.value = 0.5f;
  blender::bNode mix = {};
  blender::bNodeSocket mix_a = {};
  blender::bNodeSocket mix_b = {};
  blender::bNodeSocket mix_factor = {};
  blender::bNodeSocket mix_result = {};
  InitNode(mix, "LogicNativeMixValue", "Mix Value", 4);
  InitSocket(mix_a, "A", blender::SOCK_CUSTOM, nullptr, "NodeSocketLogicGeneric");
  InitSocket(mix_b, "B", blender::SOCK_CUSTOM, nullptr, "NodeSocketLogicGeneric");
  InitSocket(mix_factor, "Factor", blender::SOCK_FLOAT, &factor_default);
  InitSocket(mix_result, "Result", blender::SOCK_CUSTOM, nullptr, "NodeSocketLogicGeneric");
  Append(mix.inputs, mix_a);
  Append(mix.inputs, mix_b);
  Append(mix.inputs, mix_factor);
  Append(mix.outputs, mix_result);
  Append(tree.nodes, mix);

  blender::bNode set_rotation = {};
  blender::bNodeSocket set_flow = {};
  blender::bNodeSocket unused_value = {};
  SetObjectAttributeAuxSockets set_attribute_aux = {};
  InitSetObjectAttributeVectorNode(set_rotation,
                                   set_attribute_aux,
                                   set_flow,
                                   unused_value,
                                   "Set Rotation",
                                   5,
                                   1,
                                   nullptr);
  Append(tree.nodes, set_rotation);

  blender::bNodeSocketValueVector position_default = {};
  blender::bNode set_position = {};
  blender::bNodeSocket position_flow = {};
  blender::bNodeSocket position_value = {};
  SetObjectAttributeAuxSockets position_aux = {};
  InitSetObjectAttributeVectorNode(set_position,
                                   position_aux,
                                   position_flow,
                                   position_value,
                                   "Set Position",
                                   6,
                                   0,
                                   &position_default);
  Append(tree.nodes, set_position);

  std::array<blender::bNodeLink, 6> links = {};
  InitLink(links[0], a_node, a_out, mix, mix_a);
  InitLink(links[1], b_node, b_out, mix, mix_b);
  InitLink(links[2], on_update, update_out, set_rotation, set_flow);
  InitLink(links[3], mix, mix_result, set_rotation, set_attribute_aux.rotation);
  InitLink(links[4], on_update, update_out, set_position, position_flow);
  InitLink(links[5], mix, mix_result, set_position, position_value);
  for (blender::bNodeLink &link : links) {
    Append(tree.links, link);
  }

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  const LN_Instruction *position_instruction = nullptr;
  const LN_Instruction *rotation_instruction = nullptr;
  for (const LN_Instruction &instruction : program->GetInstructions(LN_Event::OnFixedUpdate)) {
    if (instruction.opcode != LN_OpCode::SetTransformVector) {
      continue;
    }
    if (instruction.vector_operation_channel ==
        uint8_t(LN_VectorOperationChannel::Position))
    {
      position_instruction = &instruction;
    }
    else if (instruction.vector_operation_channel ==
             uint8_t(LN_VectorOperationChannel::Orientation))
    {
      rotation_instruction = &instruction;
    }
  }
  ASSERT_NE(position_instruction, nullptr);
  ASSERT_NE(rotation_instruction, nullptr);
  EXPECT_EQ(position_instruction->vector_operation_mode,
            uint8_t(LN_VectorOperationMode::World));
  EXPECT_EQ(rotation_instruction->vector_operation_mode,
            uint8_t(LN_VectorOperationMode::World));

  const LN_VectorExpression *position = InstructionVectorExpression(*program,
                                                                     *position_instruction);
  const LN_VectorExpression *rotation = InstructionVectorExpression(*program,
                                                                     *rotation_instruction);
  ASSERT_NE(position, nullptr);
  ASSERT_NE(rotation, nullptr);
  EXPECT_EQ(position->kind, LN_VectorExpressionKind::FromGenericValue);
  EXPECT_EQ(position->output_type, LN_ValueType::Vector);
  EXPECT_EQ(rotation->kind, LN_VectorExpressionKind::FromGenericValue);
  EXPECT_EQ(rotation->output_type, LN_ValueType::Rotation);
  EXPECT_EQ(position->input0, rotation->input0);
  const LN_ValueExpression *mix_expression = ValueExpressionAt(*program, rotation->input0);
  ASSERT_NE(mix_expression, nullptr);
  EXPECT_EQ(mix_expression->kind, LN_ValueExpressionKind::Mix);
}

TEST(LN_TreeCompiler, EmitsGetBoneAttributeWorldSpaceFlag)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueString bone_name_default = {};
  SetCString(bone_name_default.value, "Root");
  blender::bNode get_bone = {};
  blender::bNodeSocket bone_object = {};
  blender::bNodeSocket bone_name = {};
  blender::bNodeSocket value = {};
  InitNode(get_bone, "LogicNativeGetBoneAttribute", "Get Bone Attribute", 2);
  get_bone.custom1 = 9;
  get_bone.custom2 = 1;
  InitSocket(bone_object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(bone_name, "Bone Name", blender::SOCK_STRING, &bone_name_default);
  InitSocket(value, "Value", blender::SOCK_STRING, nullptr);
  Append(get_bone.inputs, bone_object);
  Append(get_bone.inputs, bone_name);
  Append(get_bone.outputs, value);
  Append(tree.nodes, get_bone);

  blender::bNode print_node = {};
  blender::bNodeSocket print_flow = {};
  blender::bNodeSocket print_message = {};
  InitNode(print_node, "LogicNativePrint", "Print", 3);
  InitSocket(print_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(print_message, "Message", blender::SOCK_CUSTOM, nullptr);
  Append(print_node.inputs, print_flow);
  Append(print_node.inputs, print_message);
  Append(tree.nodes, print_node);

  blender::bNodeLink event_to_print = {};
  blender::bNodeLink value_to_print = {};
  InitLink(event_to_print, on_update, on_update_pulse, print_node, print_flow);
  InitLink(value_to_print, get_bone, value, print_node, print_message);
  Append(tree.links, event_to_print);
  Append(tree.links, value_to_print);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors());

  const LN_Instruction *print_instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnUpdate), LN_OpCode::Print);
  ASSERT_NE(print_instruction, nullptr);
  const LN_ValueExpression *value_expr = ValueExpressionAt(*program,
                                                           print_instruction->value_expr_index);
  ASSERT_NE(value_expr, nullptr);
  EXPECT_EQ(value_expr->kind, LN_ValueExpressionKind::BoneAttribute);
  EXPECT_EQ(value_expr->property_ref_index & LN_BONE_ATTRIBUTE_INDEX_MASK, 9u);
  EXPECT_NE(value_expr->property_ref_index & LN_BONE_ATTRIBUTE_WORLD_SPACE_FLAG, 0u);
}

TEST(LN_TreeCompiler, EmitsGetBonePoseRotationSpace)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueString bone_name_default = {};
  SetCString(bone_name_default.value, "Root");
  blender::bNode get_bone = {};
  blender::bNodeSocket bone_object = {};
  blender::bNodeSocket bone_name = {};
  blender::bNodeSocket rotation = {};
  InitNode(get_bone, "LogicNativeGetBonePoseRotation", "Get Bone Pose Rotation", 2);
  get_bone.custom1 = int(LN_BonePoseRotationSpace::World);
  InitSocket(bone_object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(bone_name, "Bone Name", blender::SOCK_STRING, &bone_name_default);
  InitSocket(rotation, "Rotation", blender::SOCK_ROTATION, nullptr);
  Append(get_bone.inputs, bone_object);
  Append(get_bone.inputs, bone_name);
  Append(get_bone.outputs, rotation);
  Append(tree.nodes, get_bone);

  blender::bNode print_node = {};
  blender::bNodeSocket print_flow = {};
  blender::bNodeSocket print_message = {};
  InitNode(print_node, "LogicNativePrint", "Print", 3);
  InitSocket(print_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(print_message, "Message", blender::SOCK_CUSTOM, nullptr);
  Append(print_node.inputs, print_flow);
  Append(print_node.inputs, print_message);
  Append(tree.nodes, print_node);

  blender::bNodeLink event_to_print = {};
  blender::bNodeLink rotation_to_print = {};
  InitLink(event_to_print, on_update, on_update_pulse, print_node, print_flow);
  InitLink(rotation_to_print, get_bone, rotation, print_node, print_message);
  Append(tree.links, event_to_print);
  Append(tree.links, rotation_to_print);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());

  const LN_Instruction *print_instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnUpdate), LN_OpCode::Print);
  ASSERT_NE(print_instruction, nullptr);
  const LN_ValueExpression *value_expr = ValueExpressionAt(*program,
                                                           print_instruction->value_expr_index);
  ASSERT_NE(value_expr, nullptr);
  EXPECT_EQ(value_expr->kind, LN_ValueExpressionKind::BonePoseRotation);
  EXPECT_EQ(value_expr->property_ref_index, uint32_t(LN_BonePoseRotationSpace::World));
  EXPECT_EQ(value_expr->input0, LN_INVALID_INDEX);
  EXPECT_NE(value_expr->string_expr_index, LN_INVALID_INDEX);
}

TEST(LN_TreeCompiler, CompilesNativeBoneRotationToObjectRotationAsTypedExpression)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueString bone_name_default = {};
  SetCString(bone_name_default.value, "Root");
  blender::bNode get_bone = {};
  blender::bNodeSocket bone_object = {};
  blender::bNodeSocket bone_name = {};
  blender::bNodeSocket native_rotation = {};
  InitNode(get_bone, "LogicNativeGetBonePoseRotation", "Get Bone Pose Rotation", 2);
  get_bone.custom1 = int(LN_BonePoseRotationSpace::Armature);
  InitSocket(bone_object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(bone_name, "Bone Name", blender::SOCK_STRING, &bone_name_default);
  InitSocket(native_rotation, "Rotation", blender::SOCK_ROTATION, nullptr);
  Append(get_bone.inputs, bone_object);
  Append(get_bone.inputs, bone_name);
  Append(get_bone.outputs, native_rotation);
  Append(tree.nodes, get_bone);

  blender::bNode converter = {};
  blender::bNodeSocket converter_input = {};
  blender::bNodeSocket object_rotation = {};
  InitNode(
      converter, "LogicNativeBoneRotationToObjectRotation", "Bone Rotation to Object Rotation", 3);
  InitSocket(converter_input, "Bone Rotation", blender::SOCK_ROTATION, nullptr);
  InitSocket(object_rotation, "Object Rotation", blender::SOCK_ROTATION, nullptr);
  Append(converter.inputs, converter_input);
  Append(converter.outputs, object_rotation);
  Append(tree.nodes, converter);

  blender::bNode print_node = {};
  blender::bNodeSocket print_flow = {};
  blender::bNodeSocket print_message = {};
  InitNode(print_node, "LogicNativePrint", "Print", 4);
  InitSocket(print_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(print_message, "Message", blender::SOCK_CUSTOM, nullptr);
  Append(print_node.inputs, print_flow);
  Append(print_node.inputs, print_message);
  Append(tree.nodes, print_node);

  blender::bNodeLink event_to_print = {};
  blender::bNodeLink bone_to_converter = {};
  blender::bNodeLink converter_to_print = {};
  InitLink(event_to_print, on_update, on_update_pulse, print_node, print_flow);
  InitLink(bone_to_converter, get_bone, native_rotation, converter, converter_input);
  InitLink(converter_to_print, converter, object_rotation, print_node, print_message);
  Append(tree.links, event_to_print);
  Append(tree.links, bone_to_converter);
  Append(tree.links, converter_to_print);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());

  const LN_Instruction *print_instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnUpdate), LN_OpCode::Print);
  ASSERT_NE(print_instruction, nullptr);
  const LN_ValueExpression *printed_value = ValueExpressionAt(*program,
                                                              print_instruction->value_expr_index);
  ASSERT_NE(printed_value, nullptr);
  EXPECT_EQ(printed_value->kind, LN_ValueExpressionKind::FromRotation);

  const LN_VectorExpression *conversion = VectorExpressionAt(*program, printed_value->input0);
  ASSERT_NE(conversion, nullptr);
  EXPECT_EQ(conversion->kind, LN_VectorExpressionKind::BoneRotationToObjectRotation);
  EXPECT_EQ(conversion->output_type, LN_ValueType::Rotation);

  const LN_VectorExpression *native_input = VectorExpressionAt(*program, conversion->input0);
  ASSERT_NE(native_input, nullptr);
  EXPECT_EQ(native_input->kind, LN_VectorExpressionKind::FromGenericValue);
  EXPECT_EQ(native_input->output_type, LN_ValueType::Rotation);

  const LN_ValueExpression *bone_rotation = ValueExpressionAt(*program, native_input->input0);
  ASSERT_NE(bone_rotation, nullptr);
  EXPECT_EQ(bone_rotation->kind, LN_ValueExpressionKind::BonePoseRotation);
  EXPECT_EQ(bone_rotation->property_ref_index, uint32_t(LN_BonePoseRotationSpace::Armature));
}

TEST(LN_TreeCompiler, CompilesGetBonePoseTransformRotationAsTypedRotation)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueString bone_name_default = {};
  SetCString(bone_name_default.value, "Root");
  blender::bNode get_transform = {};
  blender::bNodeSocket bone_object = {};
  blender::bNodeSocket bone_name = {};
  blender::bNodeSocket location = {};
  blender::bNodeSocket rotation = {};
  blender::bNodeSocket scale = {};
  InitNode(get_transform, "LogicNativeGetBonePoseTransform", "Get Bone Pose Transform", 2);
  get_transform.custom1 = int(LN_BonePoseRotationSpace::World);
  InitSocket(bone_object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(bone_name, "Bone Name", blender::SOCK_STRING, &bone_name_default);
  InitSocket(location, "Location", blender::SOCK_VECTOR, nullptr);
  InitSocket(rotation, "Rotation", blender::SOCK_ROTATION, nullptr);
  InitSocket(scale, "Scale", blender::SOCK_VECTOR, nullptr);
  Append(get_transform.inputs, bone_object);
  Append(get_transform.inputs, bone_name);
  Append(get_transform.outputs, location);
  Append(get_transform.outputs, rotation);
  Append(get_transform.outputs, scale);
  Append(tree.nodes, get_transform);

  blender::bNode separate = {};
  blender::bNodeSocket separate_rotation = {};
  blender::bNodeSocket separate_x = {};
  InitNode(separate, "LogicNativeSeparateEuler", "Separate Rotation", 3);
  InitSocket(separate_rotation, "Rotation", blender::SOCK_ROTATION, nullptr);
  InitSocket(separate_x, "X", blender::SOCK_FLOAT, nullptr);
  Append(separate.inputs, separate_rotation);
  Append(separate.outputs, separate_x);
  Append(tree.nodes, separate);

  blender::bNodeSocketValueString property_name_default = {};
  SetCString(property_name_default.value, "bone_rotation_x");
  blender::bNode set_property = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket object = {};
  blender::bNodeSocket property = {};
  blender::bNodeSocket value = {};
  InitNode(set_property, "LogicNativeSetGamePropertyFloat", "Store Bone Rotation", 4);
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(property, "Property", blender::SOCK_STRING, &property_name_default);
  InitSocket(value, "Value", blender::SOCK_FLOAT, nullptr);
  Append(set_property.inputs, flow);
  Append(set_property.inputs, object);
  Append(set_property.inputs, property);
  Append(set_property.inputs, value);
  Append(tree.nodes, set_property);

  blender::bNodeLink event_to_setter = {};
  blender::bNodeLink rotation_to_separate = {};
  blender::bNodeLink component_to_setter = {};
  InitLink(event_to_setter, on_update, on_update_pulse, set_property, flow);
  InitLink(rotation_to_separate, get_transform, rotation, separate, separate_rotation);
  InitLink(component_to_setter, separate, separate_x, set_property, value);
  Append(tree.links, event_to_setter);
  Append(tree.links, rotation_to_separate);
  Append(tree.links, component_to_setter);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());

  const LN_Instruction *instruction = FindInstruction(program->GetInstructions(LN_Event::OnUpdate),
                                                      LN_OpCode::SetGameProperty);
  ASSERT_NE(instruction, nullptr);
  const LN_FloatExpression *component = FloatExpressionAt(*program, instruction->float_expr_index);
  ASSERT_NE(component, nullptr);
  EXPECT_EQ(component->kind, LN_FloatExpressionKind::VectorComponent);
  const LN_VectorExpression *typed_rotation = VectorExpressionAt(*program, component->input0);
  ASSERT_NE(typed_rotation, nullptr);
  EXPECT_EQ(typed_rotation->output_type, LN_ValueType::Rotation);
}

TEST(LN_TreeCompiler, OnGameLoadedRoutesDestinationFlowAndExposesLoadResult)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode loaded = {};
  blender::bNodeSocket loaded_out = {};
  blender::bNodeSocket loaded_success = {};
  InitNode(loaded, "LogicNativeOnGameLoaded", "On Game Loaded", 1);
  InitSocket(loaded_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(loaded_success, "Success", blender::SOCK_BOOLEAN, nullptr);
  Append(loaded.outputs, loaded_out);
  Append(loaded.outputs, loaded_success);
  Append(tree.nodes, loaded);

  blender::bNode branch = {};
  blender::bNodeSocket branch_flow = {};
  blender::bNodeSocket branch_condition = {};
  blender::bNodeSocket branch_true = {};
  blender::bNodeSocket branch_false = {};
  InitNode(branch, "LogicNativeBranch", "Branch", 2);
  InitSocket(branch_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(branch_condition, "Condition", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(branch_true, "True", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(branch_false, "False", blender::SOCK_BOOLEAN, nullptr);
  Append(branch.inputs, branch_flow);
  Append(branch.inputs, branch_condition);
  Append(branch.outputs, branch_true);
  Append(branch.outputs, branch_false);
  Append(tree.nodes, branch);

  blender::bNodeSocketValueString message_default = {};
  SetCString(message_default.value, "checkpoint restored");
  blender::bNode print_node = {};
  blender::bNodeSocket print_flow = {};
  blender::bNodeSocket print_message = {};
  InitNode(print_node, "LogicNativePrint", "Print", 3);
  InitSocket(print_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(print_message, "Message", blender::SOCK_STRING, &message_default);
  Append(print_node.inputs, print_flow);
  Append(print_node.inputs, print_message);
  Append(tree.nodes, print_node);

  blender::bNodeLink loaded_to_branch = {};
  blender::bNodeLink success_to_branch = {};
  blender::bNodeLink branch_to_print = {};
  InitLink(loaded_to_branch, loaded, loaded_out, branch, branch_flow);
  InitLink(success_to_branch, loaded, loaded_success, branch, branch_condition);
  InitLink(branch_to_print, branch, branch_true, print_node, print_flow);
  Append(tree.links, loaded_to_branch);
  Append(tree.links, success_to_branch);
  Append(tree.links, branch_to_print);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(program, nullptr);
  EXPECT_FALSE(program->GetCompileReport().HasErrors());
  EXPECT_FALSE(
      HasWarningContaining(program->GetCompileReport(), "no On Init / On Frame Update event node"))
      << FormatCompileReport(program->GetCompileReport());
  EXPECT_NE(nullptr,
            FindInstruction(program->GetInstructions(LN_Event::OnUpdate), LN_OpCode::Print));

  bool has_load_pulse = false;
  bool has_load_success = false;
  for (const LN_BoolExpression &expression : program->GetBoolExpressions()) {
    has_load_pulse |= expression.kind == LN_BoolExpressionKind::GameLoadPulse;
    has_load_success |= expression.kind == LN_BoolExpressionKind::GameLoadSucceeded;
  }
  EXPECT_TRUE(has_load_pulse);
  EXPECT_TRUE(has_load_success);
}

TEST(LN_TreeCompiler, ReceiveEventOutDrivesFixedUpdateCommandFlow)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNodeSocketValueString subject_default = {};
  SetCString(subject_default.value, "damage");
  blender::bNode receive_event = {};
  blender::bNodeSocket receive_subject = {};
  blender::bNodeSocket receive_out = {};
  InitNode(receive_event, "LogicNativeReceiveEvent", "Receive Event", 1);
  InitSocket(receive_subject, "Subject", blender::SOCK_STRING, &subject_default);
  InitSocket(receive_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(receive_event.inputs, receive_subject);
  Append(receive_event.outputs, receive_out);
  Append(tree.nodes, receive_event);

  blender::bNodeSocketValueString message_default = {};
  SetCString(message_default.value, "received");
  blender::bNode print_node = {};
  blender::bNodeSocket print_flow = {};
  blender::bNodeSocket print_message = {};
  InitNode(print_node, "LogicNativePrint", "Print", 2);
  InitSocket(print_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(print_message, "Message", blender::SOCK_STRING, &message_default);
  Append(print_node.inputs, print_flow);
  Append(print_node.inputs, print_message);
  Append(tree.nodes, print_node);

  blender::bNodeLink receive_to_print = {};
  InitLink(receive_to_print, receive_event, receive_out, print_node, print_flow);
  Append(tree.links, receive_to_print);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());

  const LN_Instruction *print_instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::Print);
  ASSERT_NE(print_instruction, nullptr);
  ASSERT_NE(print_instruction->bool_guard_expr_index, LN_INVALID_INDEX);

  ExpectInstructionGuardedByRouteCondition(
      *program, LN_Event::OnFixedUpdate, *print_instruction, LN_BoolExpressionKind::EventReceived);
}

TEST(LN_TreeCompiler, SendEventEmptyConstantSubjectWarnsWithoutDisablingTree)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueString subject_default = {};
  blender::bNode send_event = {};
  blender::bNodeSocket send_flow = {};
  blender::bNodeSocket send_subject = {};
  blender::bNodeSocket send_content = {};
  blender::bNodeSocket send_messenger = {};
  blender::bNodeSocket send_target = {};
  blender::bNodeSocket send_done = {};
  InitNode(send_event, "LogicNativeSendEvent", "Send Event", 2);
  InitSocket(send_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(send_subject, "Subject", blender::SOCK_STRING, &subject_default);
  InitSocket(send_content, "Content", blender::SOCK_CUSTOM, nullptr);
  InitSocket(send_messenger, "Messenger", blender::SOCK_OBJECT, nullptr);
  InitSocket(send_target, "Target", blender::SOCK_OBJECT, nullptr);
  InitSocket(send_done, "Done", blender::SOCK_BOOLEAN, nullptr);
  Append(send_event.inputs, send_flow);
  Append(send_event.inputs, send_subject);
  Append(send_event.inputs, send_content);
  Append(send_event.inputs, send_messenger);
  Append(send_event.inputs, send_target);
  Append(send_event.outputs, send_done);
  Append(tree.nodes, send_event);

  blender::bNodeLink update_to_send = {};
  InitLink(update_to_send, on_update, on_update_pulse, send_event, send_flow);
  Append(tree.links, update_to_send);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  const LN_CompileReport &report = program->GetCompileReport();
  EXPECT_FALSE(report.HasErrors()) << FormatCompileReport(report);
  EXPECT_TRUE(HasWarningContaining(report, "Send Event has an empty Subject"))
      << FormatCompileReport(report);
}

TEST(LN_TreeCompiler, KeyboardDrivenEmptySendEventDoesNotDisableSiblingFlow)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNodeSocketValueString key_default = {};
  SetCString(key_default.value, "TWO");
  blender::bNode keyboard = {};
  blender::bNodeSocket key_input = {};
  blender::bNodeSocket keyboard_pressed = {};
  InitNode(keyboard, "LogicNativeKeyboardKey", "Keyboard Key", 1);
  InitSocket(key_input, "Key", blender::SOCK_STRING, &key_default);
  InitSocket(keyboard_pressed, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(keyboard.inputs, key_input);
  Append(keyboard.outputs, keyboard_pressed);
  Append(tree.nodes, keyboard);

  blender::bNodeSocketValueString empty_subject = {};
  blender::bNode send_event = {};
  blender::bNodeSocket send_flow = {};
  blender::bNodeSocket send_subject = {};
  blender::bNodeSocket send_content = {};
  blender::bNodeSocket send_messenger = {};
  blender::bNodeSocket send_target = {};
  blender::bNodeSocket send_done = {};
  InitNode(send_event, "LogicNativeSendEvent", "Send Event", 2);
  InitSocket(send_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(send_subject, "Subject", blender::SOCK_STRING, &empty_subject);
  InitSocket(send_content, "Content", blender::SOCK_CUSTOM, nullptr);
  InitSocket(send_messenger, "Messenger", blender::SOCK_OBJECT, nullptr);
  InitSocket(send_target, "Target", blender::SOCK_OBJECT, nullptr);
  InitSocket(send_done, "Done", blender::SOCK_BOOLEAN, nullptr);
  Append(send_event.inputs, send_flow);
  Append(send_event.inputs, send_subject);
  Append(send_event.inputs, send_content);
  Append(send_event.inputs, send_messenger);
  Append(send_event.inputs, send_target);
  Append(send_event.outputs, send_done);
  Append(tree.nodes, send_event);

  blender::bNodeLink keyboard_to_send = {};
  InitLink(keyboard_to_send, keyboard, keyboard_pressed, send_event, send_flow);
  Append(tree.links, keyboard_to_send);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 3);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueString message_default = {};
  SetCString(message_default.value, "sibling flow still runs");
  blender::bNode print_node = {};
  blender::bNodeSocket print_flow = {};
  blender::bNodeSocket print_message = {};
  InitNode(print_node, "LogicNativePrint", "Print", 4);
  InitSocket(print_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(print_message, "Message", blender::SOCK_STRING, &message_default);
  Append(print_node.inputs, print_flow);
  Append(print_node.inputs, print_message);
  Append(tree.nodes, print_node);

  blender::bNodeLink update_to_print = {};
  InitLink(update_to_print, on_update, on_update_pulse, print_node, print_flow);
  Append(tree.links, update_to_print);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  const LN_CompileReport &report = program->GetCompileReport();
  EXPECT_FALSE(report.HasErrors()) << FormatCompileReport(report);
  EXPECT_TRUE(HasWarningContaining(report, "Send Event has an empty Subject"))
      << FormatCompileReport(report);
  EXPECT_NE(FindInstruction(program->GetInstructions(LN_Event::OnUpdate), LN_OpCode::Print),
            nullptr);
}

TEST(LN_TreeCompiler, ReceiveEventEmptyConstantSubjectWarnsWithoutDisablingTree)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNodeSocketValueString subject_default = {};
  blender::bNode receive_event = {};
  blender::bNodeSocket receive_subject = {};
  blender::bNodeSocket receive_target = {};
  blender::bNodeSocket receive_out = {};
  InitNode(receive_event, "LogicNativeReceiveEvent", "Receive Event", 1);
  InitSocket(receive_subject, "Subject", blender::SOCK_STRING, &subject_default);
  InitSocket(receive_target, "Target", blender::SOCK_OBJECT, nullptr);
  InitSocket(receive_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(receive_event.inputs, receive_subject);
  Append(receive_event.inputs, receive_target);
  Append(receive_event.outputs, receive_out);
  Append(tree.nodes, receive_event);

  blender::bNodeSocketValueString message_default = {};
  SetCString(message_default.value, "received");
  blender::bNode print_node = {};
  blender::bNodeSocket print_flow = {};
  blender::bNodeSocket print_message = {};
  InitNode(print_node, "LogicNativePrint", "Print", 2);
  InitSocket(print_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(print_message, "Message", blender::SOCK_STRING, &message_default);
  Append(print_node.inputs, print_flow);
  Append(print_node.inputs, print_message);
  Append(tree.nodes, print_node);

  blender::bNodeLink receive_to_print = {};
  InitLink(receive_to_print, receive_event, receive_out, print_node, print_flow);
  Append(tree.links, receive_to_print);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  const LN_CompileReport &report = program->GetCompileReport();
  EXPECT_FALSE(report.HasErrors()) << FormatCompileReport(report);
  EXPECT_TRUE(HasWarningContaining(report, "Receive Event has an empty Subject"))
      << FormatCompileReport(report);

  const LN_Instruction *print_instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::Print);
  ASSERT_NE(print_instruction, nullptr);
  ASSERT_NE(print_instruction->bool_guard_expr_index, LN_INVALID_INDEX);

  ExpectInstructionGuardedByRouteCondition(
      *program, LN_Event::OnFixedUpdate, *print_instruction, LN_BoolExpressionKind::EventReceived);
}

TEST(LN_TreeCompiler, UnusedReceiveEventWithInvalidDefaultsDoesNotDisableActiveFlow)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueString message_default = {};
  SetCString(message_default.value, "still running");
  blender::bNode print_node = {};
  blender::bNodeSocket print_flow = {};
  blender::bNodeSocket print_message = {};
  InitNode(print_node, "LogicNativePrint", "Print", 2);
  InitSocket(print_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(print_message, "Message", blender::SOCK_STRING, &message_default);
  Append(print_node.inputs, print_flow);
  Append(print_node.inputs, print_message);
  Append(tree.nodes, print_node);

  blender::bNodeLink update_to_print = {};
  InitLink(update_to_print, on_update, on_update_pulse, print_node, print_flow);
  Append(tree.links, update_to_print);

  blender::bNodeSocketValueString subject_default = {};
  blender::bNodeSocketValueObject target_default = {};
  blender::bNode receive_event = {};
  blender::bNodeSocket receive_subject = {};
  blender::bNodeSocket receive_target = {};
  blender::bNodeSocket receive_out = {};
  InitNode(receive_event, "LogicNativeReceiveEvent", "Receive Event", 3);
  receive_event.custom1 = 1;
  InitSocket(receive_subject, "Subject", blender::SOCK_STRING, &subject_default);
  InitSocket(receive_target, "Target", blender::SOCK_OBJECT, &target_default);
  InitSocket(receive_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(receive_event.inputs, receive_subject);
  Append(receive_event.inputs, receive_target);
  Append(receive_event.outputs, receive_out);
  Append(tree.nodes, receive_event);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  EXPECT_NE(FindInstruction(program->GetInstructions(LN_Event::OnUpdate), LN_OpCode::Print),
            nullptr);
}

TEST(LN_TreeCompiler, SendEventUseTargetWithoutTargetWarnsAndDoesNotBroadcast)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueString subject_default = {};
  blender::bNodeSocketValueObject target_default = {};
  SetCString(subject_default.value, "damage");
  blender::bNode send_event = {};
  blender::bNodeSocket send_flow = {};
  blender::bNodeSocket send_subject = {};
  blender::bNodeSocket send_content = {};
  blender::bNodeSocket send_messenger = {};
  blender::bNodeSocket send_target = {};
  blender::bNodeSocket send_done = {};
  InitNode(send_event, "LogicNativeSendEvent", "Send Event", 2);
  send_event.custom2 = 1;
  InitSocket(send_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(send_subject, "Subject", blender::SOCK_STRING, &subject_default);
  InitSocket(send_content, "Content", blender::SOCK_CUSTOM, nullptr);
  InitSocket(send_messenger, "Messenger", blender::SOCK_OBJECT, nullptr);
  InitSocket(send_target, "Target", blender::SOCK_OBJECT, &target_default);
  InitSocket(send_done, "Done", blender::SOCK_BOOLEAN, nullptr);
  Append(send_event.inputs, send_flow);
  Append(send_event.inputs, send_subject);
  Append(send_event.inputs, send_content);
  Append(send_event.inputs, send_messenger);
  Append(send_event.inputs, send_target);
  Append(send_event.outputs, send_done);
  Append(tree.nodes, send_event);

  blender::bNodeLink update_to_send = {};
  InitLink(update_to_send, on_update, on_update_pulse, send_event, send_flow);
  Append(tree.links, update_to_send);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  const LN_CompileReport &report = program->GetCompileReport();
  EXPECT_FALSE(report.HasErrors()) << FormatCompileReport(report);
  EXPECT_TRUE(HasWarningContaining(report, "Send Event target mode has no Target object"))
      << FormatCompileReport(report);
  EXPECT_EQ(FindInstruction(program->GetInstructions(LN_Event::OnUpdate), LN_OpCode::SendEvent),
            nullptr);
}

TEST(LN_TreeCompiler, ReceiveEventUseTargetWithoutTargetWarnsWithoutDisablingTree)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNodeSocketValueString subject_default = {};
  blender::bNodeSocketValueObject target_default = {};
  SetCString(subject_default.value, "damage");
  blender::bNode receive_event = {};
  blender::bNodeSocket receive_subject = {};
  blender::bNodeSocket receive_target = {};
  blender::bNodeSocket receive_out = {};
  InitNode(receive_event, "LogicNativeReceiveEvent", "Receive Event", 1);
  receive_event.custom1 = 1;
  InitSocket(receive_subject, "Subject", blender::SOCK_STRING, &subject_default);
  InitSocket(receive_target, "Target", blender::SOCK_OBJECT, &target_default);
  InitSocket(receive_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(receive_event.inputs, receive_subject);
  Append(receive_event.inputs, receive_target);
  Append(receive_event.outputs, receive_out);
  Append(tree.nodes, receive_event);

  blender::bNodeSocketValueString message_default = {};
  SetCString(message_default.value, "received");
  blender::bNode print_node = {};
  blender::bNodeSocket print_flow = {};
  blender::bNodeSocket print_message = {};
  InitNode(print_node, "LogicNativePrint", "Print", 2);
  InitSocket(print_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(print_message, "Message", blender::SOCK_STRING, &message_default);
  Append(print_node.inputs, print_flow);
  Append(print_node.inputs, print_message);
  Append(tree.nodes, print_node);

  blender::bNodeLink receive_to_print = {};
  InitLink(receive_to_print, receive_event, receive_out, print_node, print_flow);
  Append(tree.links, receive_to_print);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  const LN_CompileReport &report = program->GetCompileReport();
  EXPECT_FALSE(report.HasErrors()) << FormatCompileReport(report);
  EXPECT_TRUE(HasWarningContaining(report, "Receive Event target mode has no Target object"))
      << FormatCompileReport(report);

  const LN_Instruction *print_instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::Print);
  ASSERT_NE(print_instruction, nullptr);
  ASSERT_NE(print_instruction->bool_guard_expr_index, LN_INVALID_INDEX);

  ExpectInstructionGuardedByRouteCondition(
      *program, LN_Event::OnFixedUpdate, *print_instruction, LN_BoolExpressionKind::EventReceived);
}

TEST(LN_TreeCompiler, SendEventAdvancedOffIgnoresHiddenMessengerSocket)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::Object messenger_object = {};
  SetCString(messenger_object.id.name, "OBMessenger");

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueString subject_default = {};
  blender::bNodeSocketValueObject messenger_default = {};
  SetCString(subject_default.value, "damage");
  messenger_default.value = &messenger_object;
  blender::bNode send_event = {};
  blender::bNodeSocket send_flow = {};
  blender::bNodeSocket send_subject = {};
  blender::bNodeSocket send_content = {};
  blender::bNodeSocket send_messenger = {};
  blender::bNodeSocket send_target = {};
  blender::bNodeSocket send_done = {};
  InitNode(send_event, "LogicNativeSendEvent", "Send Event", 2);
  send_event.custom1 = 0;
  InitSocket(send_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(send_subject, "Subject", blender::SOCK_STRING, &subject_default);
  InitSocket(send_content, "Content", blender::SOCK_CUSTOM, nullptr);
  InitSocket(send_messenger, "Messenger", blender::SOCK_OBJECT, &messenger_default);
  InitSocket(send_target, "Target", blender::SOCK_OBJECT, nullptr);
  InitSocket(send_done, "Done", blender::SOCK_BOOLEAN, nullptr);
  Append(send_event.inputs, send_flow);
  Append(send_event.inputs, send_subject);
  Append(send_event.inputs, send_content);
  Append(send_event.inputs, send_messenger);
  Append(send_event.inputs, send_target);
  Append(send_event.outputs, send_done);
  Append(tree.nodes, send_event);

  blender::bNodeLink update_to_send = {};
  InitLink(update_to_send, on_update, on_update_pulse, send_event, send_flow);
  Append(tree.links, update_to_send);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());

  const LN_Instruction *instruction = FindInstruction(program->GetInstructions(LN_Event::OnUpdate),
                                                      LN_OpCode::SendEvent);
  ASSERT_NE(instruction, nullptr);
  EXPECT_EQ(instruction->secondary_value_expr_index, LN_INVALID_INDEX);
}

TEST(LN_TreeCompiler, ValueChangedToDirectlyDrivesPrintFlow)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNodeSocketValueFloat current_default = {};
  current_default.value = 0.0f;
  blender::bNode current = {};
  blender::bNodeSocket current_output = {};
  InitNode(current, "LogicNativeValueFloat", "Current", 1);
  InitSocket(current_output, "Float", blender::SOCK_FLOAT, &current_default);
  Append(current.outputs, current_output);
  Append(tree.nodes, current);

  blender::bNodeSocketValueFloat target_default = {};
  target_default.value = 1.0f;
  blender::bNode target = {};
  blender::bNodeSocket target_output = {};
  InitNode(target, "LogicNativeValueFloat", "Target", 2);
  InitSocket(target_output, "Float", blender::SOCK_FLOAT, &target_default);
  Append(target.outputs, target_output);
  Append(tree.nodes, target);

  blender::bNode changed_to = {};
  blender::bNodeSocket changed_value = {};
  blender::bNodeSocket changed_target = {};
  blender::bNodeSocket changed_result = {};
  InitNode(changed_to, "LogicNativeValueChangedTo", "On Value Changed To", 3);
  InitSocket(changed_value, "Value", blender::SOCK_CUSTOM, nullptr);
  InitSocket(changed_target, "Target", blender::SOCK_CUSTOM, nullptr);
  InitSocket(changed_result, "Result", blender::SOCK_BOOLEAN, nullptr);
  Append(changed_to.inputs, changed_value);
  Append(changed_to.inputs, changed_target);
  Append(changed_to.outputs, changed_result);
  Append(tree.nodes, changed_to);

  blender::bNode print_node = {};
  blender::bNodeSocket print_flow = {};
  blender::bNodeSocket print_message = {};
  InitNode(print_node, "LogicNativePrint", "Print", 4);
  InitSocket(print_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(print_message, "Message", blender::SOCK_CUSTOM, nullptr);
  Append(print_node.inputs, print_flow);
  Append(print_node.inputs, print_message);
  Append(tree.nodes, print_node);

  blender::bNodeLink current_to_changed = {};
  blender::bNodeLink target_to_changed = {};
  blender::bNodeLink changed_to_print = {};
  blender::bNodeLink current_to_print = {};
  InitLink(current_to_changed, current, current_output, changed_to, changed_value);
  InitLink(target_to_changed, target, target_output, changed_to, changed_target);
  InitLink(changed_to_print, changed_to, changed_result, print_node, print_flow);
  InitLink(current_to_print, current, current_output, print_node, print_message);
  Append(tree.links, current_to_changed);
  Append(tree.links, target_to_changed);
  Append(tree.links, changed_to_print);
  Append(tree.links, current_to_print);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors());

  const LN_Instruction *print_instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::Print);
  ASSERT_NE(print_instruction, nullptr);
  ASSERT_NE(print_instruction->bool_guard_expr_index, LN_INVALID_INDEX);

  ExpectInstructionGuardedByRouteCondition(
      *program, LN_Event::OnFixedUpdate, *print_instruction, LN_BoolExpressionKind::ValueChangedTo);
}

TEST(LN_TreeCompiler, ValueChangedDirectlyDrivesPrintFlow)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNodeSocketValueFloat current_default = {};
  current_default.value = 0.0f;
  blender::bNode current = {};
  blender::bNodeSocket current_output = {};
  InitNode(current, "LogicNativeValueFloat", "Current", 1);
  InitSocket(current_output, "Float", blender::SOCK_FLOAT, &current_default);
  Append(current.outputs, current_output);
  Append(tree.nodes, current);

  blender::bNode changed = {};
  blender::bNodeSocket changed_value = {};
  blender::bNodeSocket changed_result = {};
  InitNode(changed, "LogicNativeValueChanged", "On Value Changed", 2);
  changed.custom1 = 1;
  InitSocket(changed_value, "Value", blender::SOCK_CUSTOM, nullptr);
  InitSocket(changed_result, "If Changed", blender::SOCK_BOOLEAN, nullptr);
  Append(changed.inputs, changed_value);
  Append(changed.outputs, changed_result);
  Append(tree.nodes, changed);

  blender::bNode print_node = {};
  blender::bNodeSocket print_flow = {};
  blender::bNodeSocket print_message = {};
  InitNode(print_node, "LogicNativePrint", "Print", 3);
  InitSocket(print_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(print_message, "Message", blender::SOCK_CUSTOM, nullptr);
  Append(print_node.inputs, print_flow);
  Append(print_node.inputs, print_message);
  Append(tree.nodes, print_node);

  blender::bNodeLink current_to_changed = {};
  blender::bNodeLink changed_to_print = {};
  blender::bNodeLink current_to_print = {};
  InitLink(current_to_changed, current, current_output, changed, changed_value);
  InitLink(changed_to_print, changed, changed_result, print_node, print_flow);
  InitLink(current_to_print, current, current_output, print_node, print_message);
  Append(tree.links, current_to_changed);
  Append(tree.links, changed_to_print);
  Append(tree.links, current_to_print);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors());

  const LN_Instruction *print_instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::Print);
  ASSERT_NE(print_instruction, nullptr);
  ASSERT_NE(print_instruction->bool_guard_expr_index, LN_INVALID_INDEX);

  ExpectInstructionGuardedByRouteCondition(
      *program, LN_Event::OnFixedUpdate, *print_instruction, LN_BoolExpressionKind::ValueChanged);

  const auto changed_expression = std::find_if(program->GetBoolExpressions().begin(),
                                               program->GetBoolExpressions().end(),
                                               [](const LN_BoolExpression &expression) {
                                                 return expression.kind ==
                                                        LN_BoolExpressionKind::ValueChanged;
                                               });
  ASSERT_NE(changed_expression, program->GetBoolExpressions().end());
  EXPECT_TRUE(changed_expression->bool_value);
}

TEST(LN_TreeCompiler, GenericBoolInputDoesNotUseStaleNullExpressionCache)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueBoolean value_default = {};
  value_default.value = 1;
  blender::bNode value = {};
  blender::bNodeSocket value_output = {};
  InitNode(value, "LogicNativeValueBool", "Bool", 2);
  InitSocket(value_output, "Bool", blender::SOCK_BOOLEAN, &value_default);
  Append(value.outputs, value_output);
  Append(tree.nodes, value);

  blender::bNode print_node = {};
  blender::bNodeSocket print_flow = {};
  blender::bNodeSocket print_message = {};
  InitNode(print_node, "LogicNativePrint", "Print", 3);
  InitSocket(print_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(print_message, "Message", blender::SOCK_CUSTOM, nullptr);
  Append(print_node.inputs, print_flow);
  Append(print_node.inputs, print_message);
  Append(tree.nodes, print_node);

  blender::bNodeLink event_to_print = {};
  blender::bNodeLink value_to_print = {};
  InitLink(event_to_print, on_update, on_update_pulse, print_node, print_flow);
  InitLink(value_to_print, value, value_output, print_node, print_message);
  Append(tree.links, event_to_print);
  Append(tree.links, value_to_print);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors());

  const LN_Instruction *print_instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnUpdate), LN_OpCode::Print);
  ASSERT_NE(print_instruction, nullptr);
  ASSERT_LT(print_instruction->value_expr_index, program->GetValueExpressions().size());
  EXPECT_EQ(program->GetValueExpressions()[print_instruction->value_expr_index].kind,
            LN_ValueExpressionKind::FromBool);
}

TEST(LN_TreeCompiler, PrintAcceptsCollisionVectorOutputs)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueString property_default = {};
  SetCString(property_default.value, "collider_marker");
  blender::bNode collision = {};
  blender::bNodeSocket collision_object = {};
  blender::bNodeSocket collision_property = {};
  blender::bNodeSocket collision_on = {};
  blender::bNodeSocket collision_point = {};
  blender::bNodeSocket collision_normal = {};
  InitNode(collision, "LogicNativeCollision", "Is Colliding", 1);
  InitSocket(collision_object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(collision_property, "Property", blender::SOCK_STRING, &property_default);
  InitSocket(collision_on, "Colliding", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(collision_point, "Point", blender::SOCK_VECTOR, nullptr);
  InitSocket(collision_normal, "Normal", blender::SOCK_VECTOR, nullptr);
  Append(collision.inputs, collision_object);
  Append(collision.inputs, collision_property);
  Append(collision.outputs, collision_on);
  Append(collision.outputs, collision_point);
  Append(collision.outputs, collision_normal);
  Append(tree.nodes, collision);

  blender::bNode branch = {};
  blender::bNodeSocket branch_flow = {};
  blender::bNodeSocket branch_condition = {};
  blender::bNodeSocket branch_true = {};
  InitNode(branch, "LogicNativeBranch", "Branch", 4);
  InitSocket(branch_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(branch_condition, "Condition", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(branch_true, "True", blender::SOCK_BOOLEAN, nullptr);
  Append(branch.inputs, branch_flow);
  Append(branch.inputs, branch_condition);
  Append(branch.outputs, branch_true);
  Append(tree.nodes, branch);

  blender::bNode print_point = {};
  blender::bNodeSocket point_flow = {};
  blender::bNodeSocket point_value = {};
  InitNode(print_point, "LogicNativePrint", "Print Point", 2);
  InitSocket(point_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(point_value, "Value", blender::SOCK_CUSTOM, nullptr);
  SetCString(point_value.identifier, "Message");
  Append(print_point.inputs, point_flow);
  Append(print_point.inputs, point_value);
  Append(tree.nodes, print_point);

  blender::bNode print_normal = {};
  blender::bNodeSocket normal_flow = {};
  blender::bNodeSocket normal_value = {};
  InitNode(print_normal, "LogicNativePrint", "Print Normal", 3);
  InitSocket(normal_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(normal_value, "Value", blender::SOCK_CUSTOM, nullptr);
  SetCString(normal_value.identifier, "Message");
  Append(print_normal.inputs, normal_flow);
  Append(print_normal.inputs, normal_value);
  Append(tree.nodes, print_normal);

  blender::bNodeLink event_to_branch = {};
  blender::bNodeLink collision_to_branch_condition = {};
  blender::bNodeLink branch_to_point_flow = {};
  blender::bNodeLink collision_to_point_value = {};
  blender::bNodeLink branch_to_normal_flow = {};
  blender::bNodeLink collision_to_normal_value = {};
  InitLink(event_to_branch, on_update, on_update_pulse, branch, branch_flow);
  InitLink(collision_to_branch_condition, collision, collision_on, branch, branch_condition);
  InitLink(branch_to_point_flow, branch, branch_true, print_point, point_flow);
  InitLink(collision_to_point_value, collision, collision_point, print_point, point_value);
  InitLink(branch_to_normal_flow, branch, branch_true, print_normal, normal_flow);
  InitLink(collision_to_normal_value, collision, collision_normal, print_normal, normal_value);
  Append(tree.links, event_to_branch);
  Append(tree.links, collision_to_branch_condition);
  Append(tree.links, branch_to_point_flow);
  Append(tree.links, collision_to_point_value);
  Append(tree.links, branch_to_normal_flow);
  Append(tree.links, collision_to_normal_value);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());

  bool saw_point = false;
  bool saw_normal = false;
  int print_count = 0;
  for (const LN_Instruction &instruction : program->GetInstructions(LN_Event::OnUpdate)) {
    if (instruction.opcode != LN_OpCode::Print) {
      continue;
    }
    print_count++;
    const LN_ValueExpression *value_expr = ValueExpressionAt(*program,
                                                             instruction.value_expr_index);
    ASSERT_NE(value_expr, nullptr);
    EXPECT_EQ(value_expr->kind, LN_ValueExpressionKind::FromVector);
    ASSERT_LT(value_expr->input0, program->GetVectorExpressions().size());
    const LN_VectorExpression &vector_expr = program->GetVectorExpressions()[value_expr->input0];
    saw_point |= vector_expr.kind == LN_VectorExpressionKind::CollisionHitPoint;
    saw_normal |= vector_expr.kind == LN_VectorExpressionKind::CollisionHitNormal;
  }
  EXPECT_EQ(print_count, 2);
  EXPECT_TRUE(saw_point);
  EXPECT_TRUE(saw_normal);
}

TEST(LN_TreeCompiler, RemoveObjectUsesCollisionHitObjectTarget)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnFixedUpdate", "On Fixed Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNode collision = {};
  blender::bNodeSocket collision_object = {};
  blender::bNodeSocket collision_property = {};
  blender::bNodeSocket collision_on = {};
  blender::bNodeSocket collision_hit_object = {};
  InitNode(collision, "LogicNativeCollision", "Is Colliding", 1);
  InitSocket(collision_object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(collision_property, "Property", blender::SOCK_STRING, nullptr);
  InitSocket(collision_on, "Colliding", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(collision_hit_object, "Collided Object", blender::SOCK_OBJECT, nullptr);
  Append(collision.inputs, collision_object);
  Append(collision.inputs, collision_property);
  Append(collision.outputs, collision_on);
  Append(collision.outputs, collision_hit_object);
  Append(tree.nodes, collision);

  blender::bNode branch = {};
  blender::bNodeSocket branch_flow = {};
  blender::bNodeSocket branch_condition = {};
  blender::bNodeSocket branch_true = {};
  InitNode(branch, "LogicNativeBranch", "Branch", 4);
  InitSocket(branch_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(branch_condition, "Condition", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(branch_true, "True", blender::SOCK_BOOLEAN, nullptr);
  Append(branch.inputs, branch_flow);
  Append(branch.inputs, branch_condition);
  Append(branch.outputs, branch_true);
  Append(tree.nodes, branch);

  blender::bNodeSocketValueString message_default = {};
  SetCString(message_default.value, "hit");
  blender::bNode print_node = {};
  blender::bNodeSocket print_flow = {};
  blender::bNodeSocket print_message = {};
  blender::bNodeSocket print_done = {};
  InitNode(print_node, "LogicNativePrint", "Print", 2);
  InitSocket(print_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(print_message, "Value", blender::SOCK_STRING, &message_default);
  SetCString(print_message.identifier, "Message");
  InitSocket(print_done, "Done", blender::SOCK_BOOLEAN, nullptr);
  Append(print_node.inputs, print_flow);
  Append(print_node.inputs, print_message);
  Append(print_node.outputs, print_done);
  Append(tree.nodes, print_node);

  blender::bNode remove_object = {};
  blender::bNodeSocket remove_flow = {};
  blender::bNodeSocket remove_target = {};
  InitNode(remove_object, "LogicNativeRemoveObject", "Remove Object", 3);
  InitSocket(remove_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(remove_target, "Object", blender::SOCK_OBJECT, nullptr);
  Append(remove_object.inputs, remove_flow);
  Append(remove_object.inputs, remove_target);
  Append(tree.nodes, remove_object);

  blender::bNodeLink event_to_branch = {};
  blender::bNodeLink collision_to_branch_condition = {};
  blender::bNodeLink branch_to_print = {};
  blender::bNodeLink print_to_remove = {};
  blender::bNodeLink collision_object_to_remove = {};
  InitLink(event_to_branch, on_update, on_update_pulse, branch, branch_flow);
  InitLink(collision_to_branch_condition, collision, collision_on, branch, branch_condition);
  InitLink(branch_to_print, branch, branch_true, print_node, print_flow);
  InitLink(print_to_remove, print_node, print_done, remove_object, remove_flow);
  InitLink(
      collision_object_to_remove, collision, collision_hit_object, remove_object, remove_target);
  Append(tree.links, event_to_branch);
  Append(tree.links, collision_to_branch_condition);
  Append(tree.links, branch_to_print);
  Append(tree.links, print_to_remove);
  Append(tree.links, collision_object_to_remove);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());

  const LN_Instruction *remove_instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::RemoveObject);
  ASSERT_NE(remove_instruction, nullptr);
  const LN_ValueExpression *target_expr = ValueExpressionAt(*program,
                                                            remove_instruction->value_expr_index);
  ASSERT_NE(target_expr, nullptr);
  EXPECT_EQ(target_expr->kind, LN_ValueExpressionKind::CollisionHitObject);
  const LN_Instruction *route_instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::BranchRoute);
  ASSERT_NE(route_instruction, nullptr);
  EXPECT_TRUE(BoolExpressionTreeContainsKind(
      *program, route_instruction->bool_expr_index, LN_BoolExpressionKind::CollisionDetected));
}

TEST(LN_TreeCompiler, OnCollisionEnterCompilesAsFixedUpdateGuard)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode collision = {};
  blender::bNodeSocket collision_object = {};
  blender::bNodeSocket collision_property = {};
  blender::bNodeSocket collision_enter = {};
  InitNode(collision, "LogicNativeOnCollision", "On Collision", 1);
  InitSocket(collision_object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(collision_property, "Property", blender::SOCK_STRING, nullptr);
  InitSocket(collision_enter, "On Enter", blender::SOCK_BOOLEAN, nullptr);
  Append(collision.inputs, collision_object);
  Append(collision.inputs, collision_property);
  Append(collision.outputs, collision_enter);
  Append(tree.nodes, collision);

  blender::bNodeSocketValueString message_default = {};
  SetCString(message_default.value, "hit");
  blender::bNode print_node = {};
  blender::bNodeSocket print_flow = {};
  blender::bNodeSocket print_message = {};
  InitNode(print_node, "LogicNativePrint", "Print", 2);
  InitSocket(print_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(print_message, "Value", blender::SOCK_STRING, &message_default);
  SetCString(print_message.identifier, "Message");
  Append(print_node.inputs, print_flow);
  Append(print_node.inputs, print_message);
  Append(tree.nodes, print_node);

  blender::bNodeLink collision_to_print = {};
  InitLink(collision_to_print, collision, collision_enter, print_node, print_flow);
  Append(tree.links, collision_to_print);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  EXPECT_FALSE(HasWarningContaining(program->GetCompileReport(), "will not run"))
      << FormatCompileReport(program->GetCompileReport());
  EXPECT_TRUE(HasWarningContaining(program->GetCompileReport(), "flow still runs"))
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  const LN_Instruction *print_instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::Print);
  ASSERT_NE(print_instruction, nullptr);
  ASSERT_NE(print_instruction->bool_guard_expr_index, LN_INVALID_INDEX);
  const LN_BoolExpression *guard = BoolExpressionAt(*program,
                                                    print_instruction->bool_guard_expr_index);
  ASSERT_NE(guard, nullptr);
  EXPECT_EQ(guard->kind, LN_BoolExpressionKind::InstructionExecuted);
  EXPECT_NE(FindBranchRouteWithConditionKind(*program,
                                             program->GetInstructions(LN_Event::OnFixedUpdate),
                                             LN_BoolExpressionKind::CollisionEnter),
            nullptr);
}

TEST(LN_TreeCompiler, OnConstraintBreakCompilesFixedEventAndPayloads)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNodeSocketValueString constraint_name_default = {};
  SetCString(constraint_name_default.value, "breakable_joint");
  blender::bNodeSocketValueBoolean match_any_default = {};
  match_any_default.value = true;
  blender::bNode constraint_break = {};
  blender::bNodeSocket object = {};
  blender::bNodeSocket constraint_name = {};
  blender::bNodeSocket match_any = {};
  blender::bNodeSocket broken = {};
  blender::bNodeSocket broken_constraint_name = {};
  blender::bNodeSocket target_object = {};
  blender::bNodeSocket anchor_position = {};
  blender::bNodeSocket linear_impulse = {};
  blender::bNodeSocket angular_impulse = {};
  blender::bNodeSocket applied_impulse = {};
  blender::bNodeSocket break_count = {};
  blender::bNodeSocket break_events = {};
  InitNode(constraint_break, "LogicNativeOnConstraintBreak", "On Constraint Break", 1);
  constraint_break.custom1 = int(LN_RigidBodyConstraintMatchMode::Contains);
  InitSocket(object, "Object", blender::SOCK_OBJECT, nullptr, "NodeSocketLogicObject");
  InitSocket(constraint_name,
             "Constraint Name",
             blender::SOCK_STRING,
             &constraint_name_default,
             "NodeSocketLogicString");
  InitSocket(
      match_any, "Match Any", blender::SOCK_BOOLEAN, &match_any_default, "NodeSocketLogicBool");
  InitSocket(broken, "Broken", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  InitSocket(broken_constraint_name,
             "Constraint Name",
             blender::SOCK_STRING,
             nullptr,
             "NodeSocketLogicString");
  InitSocket(
      target_object, "Target Object", blender::SOCK_OBJECT, nullptr, "NodeSocketLogicObject");
  InitSocket(
      anchor_position, "Anchor Position", blender::SOCK_VECTOR, nullptr, "NodeSocketLogicVector");
  InitSocket(
      linear_impulse, "Linear Impulse", blender::SOCK_FLOAT, nullptr, "NodeSocketLogicFloat");
  InitSocket(
      angular_impulse, "Angular Impulse", blender::SOCK_FLOAT, nullptr, "NodeSocketLogicFloat");
  InitSocket(
      applied_impulse, "Applied Impulse", blender::SOCK_FLOAT, nullptr, "NodeSocketLogicFloat");
  InitSocket(break_count, "Break Count", blender::SOCK_INT, nullptr, "NodeSocketLogicInt");
  InitSocket(break_events, "Break Events", blender::SOCK_CUSTOM, nullptr, "NodeSocketLogicList");
  Append(constraint_break.inputs, object);
  Append(constraint_break.inputs, constraint_name);
  Append(constraint_break.inputs, match_any);
  Append(constraint_break.outputs, broken);
  Append(constraint_break.outputs, broken_constraint_name);
  Append(constraint_break.outputs, target_object);
  Append(constraint_break.outputs, anchor_position);
  Append(constraint_break.outputs, linear_impulse);
  Append(constraint_break.outputs, angular_impulse);
  Append(constraint_break.outputs, applied_impulse);
  Append(constraint_break.outputs, break_count);
  Append(constraint_break.outputs, break_events);
  Append(tree.nodes, constraint_break);

  blender::bNodeSocketValueString message_default = {};
  SetCString(message_default.value, "constraint broke");
  blender::bNode print_node = {};
  blender::bNodeSocket print_flow = {};
  blender::bNodeSocket print_message = {};
  InitNode(print_node, "LogicNativePrint", "Print", 2);
  InitSocket(print_flow, "Flow", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  InitSocket(print_message, "Value", blender::SOCK_STRING, &message_default);
  SetCString(print_message.identifier, "Message");
  Append(print_node.inputs, print_flow);
  Append(print_node.inputs, print_message);
  Append(tree.nodes, print_node);

  blender::bNodeLink break_to_print = {};
  InitLink(break_to_print, constraint_break, broken, print_node, print_flow);
  Append(tree.links, break_to_print);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  const LN_Instruction *constraint_break_route = FindBranchRouteWithConditionKind(
      *program,
      program->GetInstructions(LN_Event::OnFixedUpdate),
      LN_BoolExpressionKind::ConstraintBreak);
  ASSERT_NE(constraint_break_route, nullptr);
  const LN_BoolExpression *constraint_break_condition = BoolExpressionAt(
      *program, constraint_break_route->bool_expr_index);
  ASSERT_NE(constraint_break_condition, nullptr);
  ASSERT_NE(constraint_break_condition->input2, LN_INVALID_INDEX);
  EXPECT_EQ(constraint_break_condition->rigid_body_constraint_match_mode,
            LN_RigidBodyConstraintMatchMode::Contains);
  const LN_BoolExpression *match_any_expression = BoolExpressionAt(
      *program, constraint_break_condition->input2);
  ASSERT_NE(match_any_expression, nullptr);
  EXPECT_EQ(match_any_expression->kind, LN_BoolExpressionKind::Constant);
  EXPECT_TRUE(match_any_expression->bool_value);

  const auto has_float_expression = [&](const LN_FloatExpressionKind kind) {
    return std::any_of(
        program->GetFloatExpressions().begin(),
        program->GetFloatExpressions().end(),
        [&](const LN_FloatExpression &expression) { return expression.kind == kind; });
  };
  EXPECT_TRUE(has_float_expression(LN_FloatExpressionKind::ConstraintBreakLinearImpulse));
  EXPECT_TRUE(has_float_expression(LN_FloatExpressionKind::ConstraintBreakAngularImpulse));
  EXPECT_TRUE(has_float_expression(LN_FloatExpressionKind::ConstraintBreakAppliedImpulse));
  for (const LN_FloatExpression &expression : program->GetFloatExpressions()) {
    if (expression.kind == LN_FloatExpressionKind::ConstraintBreakLinearImpulse ||
        expression.kind == LN_FloatExpressionKind::ConstraintBreakAngularImpulse ||
        expression.kind == LN_FloatExpressionKind::ConstraintBreakAppliedImpulse)
    {
      EXPECT_EQ(expression.rigid_body_constraint_match_mode,
                LN_RigidBodyConstraintMatchMode::Contains);
    }
  }
  const auto constraint_break_count = std::find_if(
      program->GetIntExpressions().begin(),
      program->GetIntExpressions().end(),
      [](const LN_IntExpression &expression) {
        return expression.kind == LN_IntExpressionKind::ConstraintBreakCount;
      });
  ASSERT_NE(constraint_break_count, program->GetIntExpressions().end());
  EXPECT_EQ(constraint_break_count->rigid_body_constraint_match_mode,
            LN_RigidBodyConstraintMatchMode::Contains);
  const auto constraint_break_name = std::find_if(
      program->GetStringExpressions().begin(),
      program->GetStringExpressions().end(),
      [](const LN_StringExpression &expression) {
        return expression.kind == LN_StringExpressionKind::ConstraintBreakName;
      });
  ASSERT_NE(constraint_break_name, program->GetStringExpressions().end());
  EXPECT_EQ(constraint_break_name->rigid_body_constraint_match_mode,
            LN_RigidBodyConstraintMatchMode::Contains);
  const auto constraint_break_anchor = std::find_if(
      program->GetVectorExpressions().begin(),
      program->GetVectorExpressions().end(),
      [](const LN_VectorExpression &expression) {
        return expression.kind == LN_VectorExpressionKind::ConstraintBreakAnchorPosition;
      });
  ASSERT_NE(constraint_break_anchor, program->GetVectorExpressions().end());
  EXPECT_EQ(constraint_break_anchor->rigid_body_constraint_match_mode,
            LN_RigidBodyConstraintMatchMode::Contains);
  const auto target_object_expression = std::find_if(
      program->GetValueExpressions().begin(),
      program->GetValueExpressions().end(),
      [](const LN_ValueExpression &expression) {
        return expression.kind == LN_ValueExpressionKind::ConstraintBreakTargetObject;
      });
  ASSERT_NE(target_object_expression, program->GetValueExpressions().end());
  EXPECT_EQ(target_object_expression->rigid_body_constraint_match_mode,
            LN_RigidBodyConstraintMatchMode::Contains);
  const auto break_events_expression = std::find_if(
      program->GetValueExpressions().begin(),
      program->GetValueExpressions().end(),
      [](const LN_ValueExpression &expression) {
        return expression.kind == LN_ValueExpressionKind::ConstraintBreakEvents;
      });
  ASSERT_NE(break_events_expression, program->GetValueExpressions().end());
  EXPECT_EQ(break_events_expression->rigid_body_constraint_match_mode,
            LN_RigidBodyConstraintMatchMode::Contains);
}

TEST(LN_TreeCompiler, OnCollisionCollidingConditionCompilesAsPrintValue)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode collision = {};
  blender::bNodeSocket collision_object = {};
  blender::bNodeSocket collision_property = {};
  blender::bNodeSocket collision_material = {};
  blender::bNodeSocket collision_stay = {};
  blender::bNodeSocket collision_colliding = {};
  InitNode(collision, "LogicNativeOnCollision", "On Collision", 1);
  InitSocket(collision_object, "Object", blender::SOCK_OBJECT, nullptr, "NodeSocketLogicObject");
  InitSocket(
      collision_property, "Property", blender::SOCK_STRING, nullptr, "NodeSocketLogicString");
  InitSocket(
      collision_material, "Material", blender::SOCK_MATERIAL, nullptr, "NodeSocketLogicMaterial");
  InitSocket(
      collision_stay, "On Stay", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  InitSocket(collision_colliding,
             "Colliding",
             blender::SOCK_BOOLEAN,
             nullptr,
             "NodeSocketLogicCondition");
  Append(collision.inputs, collision_object);
  Append(collision.inputs, collision_property);
  Append(collision.inputs, collision_material);
  Append(collision.outputs, collision_stay);
  Append(collision.outputs, collision_colliding);
  Append(tree.nodes, collision);

  blender::bNode print_node = {};
  blender::bNodeSocket print_flow = {};
  blender::bNodeSocket print_value = {};
  InitNode(print_node, "LogicNativePrint", "Print", 2);
  InitSocket(print_flow, "Flow", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  InitSocketWithIdentifier(
      print_value, "Message", "Value", blender::SOCK_CUSTOM, nullptr, "NodeSocketLogicGeneric");
  Append(print_node.inputs, print_flow);
  Append(print_node.inputs, print_value);
  Append(tree.nodes, print_node);

  blender::bNodeLink stay_to_print = {};
  blender::bNodeLink colliding_to_print = {};
  InitLink(stay_to_print, collision, collision_stay, print_node, print_flow);
  InitLink(colliding_to_print, collision, collision_colliding, print_node, print_value);
  Append(tree.links, stay_to_print);
  Append(tree.links, colliding_to_print);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  const LN_Instruction *print_instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::Print);
  ASSERT_NE(print_instruction, nullptr);
  const LN_ValueExpression *message_expr = ValueExpressionAt(*program,
                                                             print_instruction->value_expr_index);
  ASSERT_NE(message_expr, nullptr);
  ASSERT_EQ(message_expr->kind, LN_ValueExpressionKind::FromBool);
  const LN_BoolExpression *colliding_expr = BoolExpressionAt(*program, message_expr->input0);
  ASSERT_NE(colliding_expr, nullptr);
  EXPECT_EQ(colliding_expr->kind, LN_BoolExpressionKind::CollisionDetected);
  EXPECT_NE(FindBranchRouteWithConditionKind(*program,
                                             program->GetInstructions(LN_Event::OnFixedUpdate),
                                             LN_BoolExpressionKind::CollisionStay),
            nullptr);
}

TEST(LN_TreeCompiler, CollisionCollidingConditionCompilesAsPrintValue)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNode collision = {};
  blender::bNodeSocket collision_object = {};
  blender::bNodeSocket collision_property = {};
  blender::bNodeSocket collision_material = {};
  blender::bNodeSocket collision_colliding = {};
  InitNode(collision, "LogicNativeCollision", "Is Colliding", 2);
  InitSocket(collision_object, "Object", blender::SOCK_OBJECT, nullptr, "NodeSocketLogicObject");
  InitSocket(
      collision_property, "Property", blender::SOCK_STRING, nullptr, "NodeSocketLogicString");
  InitSocket(
      collision_material, "Material", blender::SOCK_MATERIAL, nullptr, "NodeSocketLogicMaterial");
  InitSocket(collision_colliding,
             "Colliding",
             blender::SOCK_BOOLEAN,
             nullptr,
             "NodeSocketLogicCondition");
  Append(collision.inputs, collision_object);
  Append(collision.inputs, collision_property);
  Append(collision.inputs, collision_material);
  Append(collision.outputs, collision_colliding);
  Append(tree.nodes, collision);

  blender::bNode print_node = {};
  blender::bNodeSocket print_flow = {};
  blender::bNodeSocket print_value = {};
  InitNode(print_node, "LogicNativePrint", "Print", 3);
  InitSocket(print_flow, "Flow", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  InitSocketWithIdentifier(
      print_value, "Message", "Value", blender::SOCK_CUSTOM, nullptr, "NodeSocketLogicGeneric");
  Append(print_node.inputs, print_flow);
  Append(print_node.inputs, print_value);
  Append(tree.nodes, print_node);

  blender::bNodeLink update_to_print = {};
  blender::bNodeLink colliding_to_print = {};
  InitLink(update_to_print, on_update, on_update_pulse, print_node, print_flow);
  InitLink(colliding_to_print, collision, collision_colliding, print_node, print_value);
  Append(tree.links, update_to_print);
  Append(tree.links, colliding_to_print);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  const LN_Instruction *print_instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnUpdate), LN_OpCode::Print);
  ASSERT_NE(print_instruction, nullptr);
  const LN_ValueExpression *message_expr = ValueExpressionAt(*program,
                                                             print_instruction->value_expr_index);
  ASSERT_NE(message_expr, nullptr);
  ASSERT_EQ(message_expr->kind, LN_ValueExpressionKind::FromBool);
  const LN_BoolExpression *colliding_expr = BoolExpressionAt(*program, message_expr->input0);
  ASSERT_NE(colliding_expr, nullptr);
  EXPECT_EQ(colliding_expr->kind, LN_BoolExpressionKind::CollisionDetected);
}

TEST(LN_TreeCompiler, OnCollisionContactCountPrintKeepsMaterialFilter)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::Material filter_material = {};
  InitMaterial(filter_material, "ColliderRed");
  blender::bNodeSocketValueMaterial material_default = {};
  material_default.value = &filter_material;

  blender::bNode collision = {};
  blender::bNodeSocket collision_object = {};
  blender::bNodeSocket collision_property = {};
  blender::bNodeSocket collision_material = {};
  blender::bNodeSocket collision_enter = {};
  blender::bNodeSocket collision_count = {};
  InitNode(collision, "LogicNativeOnCollision", "On Collision", 1);
  InitSocket(collision_object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(collision_property, "Property", blender::SOCK_STRING, nullptr);
  InitSocket(collision_material, "Material", blender::SOCK_MATERIAL, &material_default);
  InitSocket(collision_enter, "On Enter", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(collision_count, "Contact Count", blender::SOCK_INT, nullptr);
  Append(collision.inputs, collision_object);
  Append(collision.inputs, collision_property);
  Append(collision.inputs, collision_material);
  Append(collision.outputs, collision_enter);
  Append(collision.outputs, collision_count);
  Append(tree.nodes, collision);

  blender::bNode print_node = {};
  blender::bNodeSocket print_flow = {};
  blender::bNodeSocket print_message = {};
  InitNode(print_node, "LogicNativePrint", "Print", 2);
  InitSocket(print_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(print_message, "Value", blender::SOCK_CUSTOM, nullptr);
  Append(print_node.inputs, print_flow);
  Append(print_node.inputs, print_message);
  Append(tree.nodes, print_node);

  blender::bNodeLink collision_to_print = {};
  blender::bNodeLink count_to_print = {};
  InitLink(collision_to_print, collision, collision_enter, print_node, print_flow);
  InitLink(count_to_print, collision, collision_count, print_node, print_message);
  Append(tree.links, collision_to_print);
  Append(tree.links, count_to_print);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  const LN_Instruction *print_instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::Print);
  ASSERT_NE(print_instruction, nullptr);

  const LN_ValueExpression *message_expr = ValueExpressionAt(*program,
                                                             print_instruction->value_expr_index);
  ASSERT_NE(message_expr, nullptr);
  ASSERT_EQ(message_expr->kind, LN_ValueExpressionKind::FromInt);
  const LN_IntExpression *count_expr = IntExpressionAt(*program, message_expr->input0);
  ASSERT_NE(count_expr, nullptr);
  EXPECT_EQ(count_expr->kind, LN_IntExpressionKind::CollisionContactCount);
  ASSERT_NE(count_expr->input2, LN_INVALID_INDEX);

  const LN_ValueExpression *material_expr = ValueExpressionAt(*program, count_expr->input2);
  ASSERT_NE(material_expr, nullptr);
  EXPECT_EQ(material_expr->kind, LN_ValueExpressionKind::Constant);
  EXPECT_EQ(material_expr->value.type, LN_ValueType::DatablockRef);
  EXPECT_TRUE(material_expr->value.exists);
  EXPECT_EQ(material_expr->value.reference_name, "ColliderRed");
}

TEST(LN_TreeCompiler, DoOnceCompilesResetOnItsIndependentEventPath)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_init = {};
  blender::bNodeSocket init_out = {};
  InitNode(on_init, "LogicNativeOnInit", "On Init", 1);
  InitSocket(init_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_init.outputs, init_out);
  Append(tree.nodes, on_init);

  blender::bNode on_update = {};
  blender::bNodeSocket update_out = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 2);
  InitSocket(update_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, update_out);
  Append(tree.nodes, on_update);

  blender::bNode once = {};
  blender::bNodeSocket once_flow = {};
  blender::bNodeSocket once_reset = {};
  blender::bNodeSocket once_out = {};
  InitNode(once, "LogicNativeOnce", "Do Once", 3);
  InitSocket(once_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(once_reset, "Reset", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(once_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(once.inputs, once_flow);
  Append(once.inputs, once_reset);
  Append(once.outputs, once_out);
  Append(tree.nodes, once);

  blender::bNode quit = {};
  blender::bNodeSocket quit_flow = {};
  InitNode(quit, "LogicNativeQuitGame", "Quit Game", 4);
  InitSocket(quit_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  Append(quit.inputs, quit_flow);
  Append(tree.nodes, quit);

  blender::bNodeLink init_to_reset = {};
  blender::bNodeLink update_to_flow = {};
  blender::bNodeLink once_to_quit = {};
  InitLink(init_to_reset, on_init, init_out, once, once_reset);
  InitLink(update_to_flow, on_update, update_out, once, once_flow);
  InitLink(once_to_quit, once, once_out, quit, quit_flow);
  Append(tree.links, init_to_reset);
  Append(tree.links, update_to_flow);
  Append(tree.links, once_to_quit);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  const LN_Instruction *reset = FindInstruction(program->GetInstructions(LN_Event::OnInit),
                                                LN_OpCode::ResetOnce);
  const LN_Instruction *attempt = FindInstruction(program->GetInstructions(LN_Event::OnUpdate),
                                                  LN_OpCode::TryOnce);
  ASSERT_NE(reset, nullptr);
  ASSERT_NE(attempt, nullptr);
  EXPECT_EQ(reset->int_value, attempt->int_value);
  EXPECT_EQ(FindInstruction(program->GetInstructions(LN_Event::OnInit), LN_OpCode::TryOnce),
            nullptr);

  const std::vector<LN_Instruction> &fixed = program->GetInstructions(LN_Event::OnUpdate);
  const int attempt_index = FindInstructionIndex(fixed, LN_OpCode::TryOnce);
  const int route_index = FindInstructionIndex(fixed, LN_OpCode::BranchRoute);
  const int quit_index = FindInstructionIndex(fixed, LN_OpCode::QuitGame);
  ASSERT_GE(attempt_index, 0);
  ASSERT_GT(route_index, attempt_index);
  EXPECT_GT(quit_index, route_index);
  const LN_BoolExpression *route_condition = BoolExpressionAt(
      *program, fixed[size_t(route_index)].bool_expr_index);
  ASSERT_NE(route_condition, nullptr);
  EXPECT_EQ(route_condition->kind, LN_BoolExpressionKind::Once);
}

TEST(LN_TreeCompiler, DoOnceOrdersResetBeforeAttemptOnTheSameEvent)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket update_out = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(update_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, update_out);
  Append(tree.nodes, on_update);

  blender::bNode once = {};
  blender::bNodeSocket once_flow = {};
  blender::bNodeSocket once_reset = {};
  blender::bNodeSocket once_out = {};
  InitNode(once, "LogicNativeOnce", "Do Once", 2);
  InitSocket(once_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(once_reset, "Reset", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(once_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(once.inputs, once_flow);
  Append(once.inputs, once_reset);
  Append(once.outputs, once_out);
  Append(tree.nodes, once);

  blender::bNode quit = {};
  blender::bNodeSocket quit_flow = {};
  InitNode(quit, "LogicNativeQuitGame", "Quit Game", 3);
  InitSocket(quit_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  Append(quit.inputs, quit_flow);
  Append(tree.nodes, quit);

  blender::bNodeLink update_to_flow = {};
  blender::bNodeLink update_to_reset = {};
  blender::bNodeLink once_to_quit = {};
  InitLink(update_to_flow, on_update, update_out, once, once_flow);
  InitLink(update_to_reset, on_update, update_out, once, once_reset);
  InitLink(once_to_quit, once, once_out, quit, quit_flow);
  Append(tree.links, update_to_flow);
  Append(tree.links, update_to_reset);
  Append(tree.links, once_to_quit);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  const std::vector<LN_Instruction> &fixed = program->GetInstructions(LN_Event::OnUpdate);
  const int reset_index = FindInstructionIndex(fixed, LN_OpCode::ResetOnce);
  const int attempt_index = FindInstructionIndex(fixed, LN_OpCode::TryOnce);
  ASSERT_GE(reset_index, 0);
  EXPECT_GT(attempt_index, reset_index);
  EXPECT_EQ(fixed[size_t(reset_index)].int_value, fixed[size_t(attempt_index)].int_value);
}

TEST(LN_TreeCompiler, FlowGateOrdersSameEventControlsBeforeEnterWithDocumentedPrecedence)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket update_out = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(update_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, update_out);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueBoolean start_open_default = {};
  blender::bNode gate = {};
  blender::bNodeSocket enter = {};
  blender::bNodeSocket toggle = {};
  blender::bNodeSocket open = {};
  blender::bNodeSocket close = {};
  blender::bNodeSocket start_open = {};
  blender::bNodeSocket exit = {};
  blender::bNodeSocket is_open = {};
  InitNode(gate, "LogicNativeFlowGate", "Flow Gate", 2);
  InitSocket(enter, "Enter", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(toggle, "Toggle", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(open, "Open", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(close, "Close", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(start_open, "Start Open", blender::SOCK_BOOLEAN, &start_open_default);
  InitSocket(exit, "Exit", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(is_open, "Is Open", blender::SOCK_BOOLEAN, nullptr);
  Append(gate.inputs, enter);
  Append(gate.inputs, toggle);
  Append(gate.inputs, open);
  Append(gate.inputs, close);
  Append(gate.inputs, start_open);
  Append(gate.outputs, exit);
  Append(gate.outputs, is_open);
  Append(tree.nodes, gate);

  blender::bNode quit = {};
  blender::bNodeSocket quit_flow = {};
  InitNode(quit, "LogicNativeQuitGame", "Quit Game", 3);
  InitSocket(quit_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  Append(quit.inputs, quit_flow);
  Append(tree.nodes, quit);

  blender::bNodeLink update_to_enter = {};
  blender::bNodeLink update_to_toggle = {};
  blender::bNodeLink update_to_open = {};
  blender::bNodeLink update_to_close = {};
  blender::bNodeLink exit_to_quit = {};
  InitLink(update_to_enter, on_update, update_out, gate, enter);
  InitLink(update_to_toggle, on_update, update_out, gate, toggle);
  InitLink(update_to_open, on_update, update_out, gate, open);
  InitLink(update_to_close, on_update, update_out, gate, close);
  InitLink(exit_to_quit, gate, exit, quit, quit_flow);
  /* Deliberately scramble link creation order. Compile order must remain semantic. */
  Append(tree.links, update_to_close);
  Append(tree.links, update_to_enter);
  Append(tree.links, update_to_open);
  Append(tree.links, exit_to_quit);
  Append(tree.links, update_to_toggle);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  const std::vector<LN_Instruction> &instructions = program->GetInstructions(LN_Event::OnUpdate);
  std::vector<LN_FlowGateControl> controls;
  int try_index = -1;
  int quit_index = -1;
  int32_t gate_expression = -1;
  for (int index = 0; index < int(instructions.size()); index++) {
    const LN_Instruction &instruction = instructions[size_t(index)];
    if (instruction.opcode == LN_OpCode::SetFlowGate) {
      controls.push_back(LN_FlowGateControl(instruction.secondary_int_value));
      gate_expression = instruction.int_value;
    }
    else if (instruction.opcode == LN_OpCode::TryFlowGate) {
      try_index = index;
      EXPECT_EQ(instruction.int_value, gate_expression);
    }
    else if (instruction.opcode == LN_OpCode::QuitGame) {
      quit_index = index;
    }
  }
  ASSERT_EQ(controls.size(), 3u);
  EXPECT_EQ(controls[0], LN_FlowGateControl::Toggle);
  EXPECT_EQ(controls[1], LN_FlowGateControl::Open);
  EXPECT_EQ(controls[2], LN_FlowGateControl::Close);
  ASSERT_GE(try_index, 0);
  EXPECT_GT(quit_index, try_index);
  const LN_BoolExpression *accepted = BoolExpressionAt(
      *program, instructions[size_t(quit_index)].bool_guard_expr_index);
  ASSERT_NE(accepted, nullptr);
  EXPECT_EQ(accepted->kind, LN_BoolExpressionKind::InstructionExecuted);
  EXPECT_EQ(accepted->input0, uint32_t(try_index));
  ASSERT_GE(gate_expression, 0);
  ASSERT_LT(size_t(gate_expression), program->GetBoolExpressions().size());
  EXPECT_EQ(program->GetBoolExpressions()[size_t(gate_expression)].kind,
            LN_BoolExpressionKind::FlowGateOpen);
}

TEST(LN_TreeCompiler, FlowGateHonorsExplicitSequenceOrderBetweenEnterAndControl)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket update_out = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(update_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, update_out);
  Append(tree.nodes, on_update);

  blender::bNode sequence = {};
  blender::bNodeSocket sequence_flow = {};
  std::array<blender::bNodeSocket, 4> then_outputs = {};
  InitNode(sequence, "LogicNativeSequence", "Sequence", 2);
  InitSocket(sequence_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  Append(sequence.inputs, sequence_flow);
  for (int index = 0; index < int(then_outputs.size()); index++) {
    const std::string name = "Then " + std::to_string(index);
    InitSocket(then_outputs[size_t(index)], name.c_str(), blender::SOCK_BOOLEAN, nullptr);
    Append(sequence.outputs, then_outputs[size_t(index)]);
  }
  Append(tree.nodes, sequence);

  blender::bNodeSocketValueBoolean start_open_default = {};
  blender::bNode gate = {};
  blender::bNodeSocket enter = {};
  blender::bNodeSocket toggle = {};
  blender::bNodeSocket open = {};
  blender::bNodeSocket close = {};
  blender::bNodeSocket start_open = {};
  blender::bNodeSocket exit = {};
  blender::bNodeSocket is_open = {};
  InitNode(gate, "LogicNativeFlowGate", "Flow Gate", 3);
  InitSocket(enter, "Enter", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(toggle, "Toggle", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(open, "Open", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(close, "Close", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(start_open, "Start Open", blender::SOCK_BOOLEAN, &start_open_default);
  InitSocket(exit, "Exit", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(is_open, "Is Open", blender::SOCK_BOOLEAN, nullptr);
  Append(gate.inputs, enter);
  Append(gate.inputs, toggle);
  Append(gate.inputs, open);
  Append(gate.inputs, close);
  Append(gate.inputs, start_open);
  Append(gate.outputs, exit);
  Append(gate.outputs, is_open);
  Append(tree.nodes, gate);

  blender::bNode branch = {};
  blender::bNodeSocket branch_flow = {};
  blender::bNodeSocket branch_condition = {};
  blender::bNodeSocket branch_true = {};
  blender::bNodeSocket branch_false = {};
  InitNode(branch, "LogicNativeBranch", "Branch", 4);
  InitSocket(branch_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(branch_condition, "Condition", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(branch_true, "True", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(branch_false, "False", blender::SOCK_BOOLEAN, nullptr);
  Append(branch.inputs, branch_flow);
  Append(branch.inputs, branch_condition);
  Append(branch.outputs, branch_true);
  Append(branch.outputs, branch_false);
  Append(tree.nodes, branch);

  blender::bNode quit = {};
  blender::bNodeSocket quit_flow = {};
  InitNode(quit, "LogicNativeQuitGame", "Quit Game", 5);
  InitSocket(quit_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  Append(quit.inputs, quit_flow);
  Append(tree.nodes, quit);

  blender::bNode exit_quit = {};
  blender::bNodeSocket exit_quit_flow = {};
  InitNode(exit_quit, "LogicNativeQuitGame", "Exit Quit", 6);
  InitSocket(exit_quit_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  Append(exit_quit.inputs, exit_quit_flow);
  Append(tree.nodes, exit_quit);

  blender::bNodeLink update_to_sequence = {};
  blender::bNodeLink branch_first = {};
  blender::bNodeLink state_to_branch = {};
  blender::bNodeLink branch_to_quit = {};
  blender::bNodeLink enter_first = {};
  blender::bNodeLink open_second = {};
  blender::bNodeLink exit_to_quit = {};
  InitLink(update_to_sequence, on_update, update_out, sequence, sequence_flow);
  InitLink(branch_first, sequence, then_outputs[0], branch, branch_flow);
  InitLink(state_to_branch, gate, is_open, branch, branch_condition);
  InitLink(branch_to_quit, branch, branch_true, quit, quit_flow);
  InitLink(enter_first, sequence, then_outputs[1], gate, enter);
  InitLink(open_second, sequence, then_outputs[2], gate, open);
  InitLink(exit_to_quit, gate, exit, exit_quit, exit_quit_flow);
  Append(tree.links, open_second);
  Append(tree.links, branch_to_quit);
  Append(tree.links, exit_to_quit);
  Append(tree.links, state_to_branch);
  Append(tree.links, update_to_sequence);
  Append(tree.links, enter_first);
  Append(tree.links, branch_first);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  const std::vector<LN_Instruction> &instructions = program->GetInstructions(LN_Event::OnUpdate);
  int branch_index = -1;
  int enter_index = -1;
  int open_index = -1;
  for (int index = 0; index < int(instructions.size()); index++) {
    const LN_Instruction &instruction = instructions[size_t(index)];
    if (instruction.opcode == LN_OpCode::BranchRoute &&
        instruction.bool_expr_index < program->GetBoolExpressions().size() &&
        program->GetBoolExpressions()[instruction.bool_expr_index].kind ==
            LN_BoolExpressionKind::FlowGateOpen &&
        branch_index < 0)
    {
      branch_index = index;
    }
    else if (instruction.opcode == LN_OpCode::TryFlowGate) {
      enter_index = index;
    }
    else if (instruction.opcode == LN_OpCode::SetFlowGate &&
             instruction.secondary_int_value == int32_t(LN_FlowGateControl::Open))
    {
      open_index = index;
    }
  }
  ASSERT_GE(branch_index, 0);
  ASSERT_GE(enter_index, 0);
  ASSERT_GE(open_index, 0);
  EXPECT_LT(branch_index, enter_index);
  EXPECT_LT(enter_index, open_index);
}

TEST(LN_TreeCompiler, AnyFlowPreservesIndependentEventPulses)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_init = {};
  blender::bNodeSocket init_out = {};
  InitNode(on_init, "LogicNativeOnInit", "On Init", 1);
  InitSocket(init_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_init.outputs, init_out);
  Append(tree.nodes, on_init);

  blender::bNode on_update = {};
  blender::bNodeSocket update_out = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 2);
  InitSocket(update_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, update_out);
  Append(tree.nodes, on_update);

  blender::bNode any_flow = {};
  blender::bNodeSocket any_in = {};
  blender::bNodeSocket any_out = {};
  InitNode(any_flow, "LogicNativeAnyFlow", "Any Flow", 3);
  InitSocket(any_in, "In", blender::SOCK_BOOLEAN, nullptr);
  any_in.flag |= blender::SOCK_MULTI_INPUT;
  InitSocket(any_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(any_flow.inputs, any_in);
  Append(any_flow.outputs, any_out);
  Append(tree.nodes, any_flow);

  blender::bNode quit = {};
  blender::bNodeSocket quit_flow = {};
  InitNode(quit, "LogicNativeQuitGame", "Quit Game", 4);
  InitSocket(quit_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  Append(quit.inputs, quit_flow);
  Append(tree.nodes, quit);

  blender::bNodeLink init_to_any = {};
  blender::bNodeLink update_to_any_first = {};
  blender::bNodeLink update_to_any_second = {};
  blender::bNodeLink any_to_quit = {};
  InitLink(init_to_any, on_init, init_out, any_flow, any_in);
  InitLink(update_to_any_first, on_update, update_out, any_flow, any_in);
  InitLink(update_to_any_second, on_update, update_out, any_flow, any_in);
  InitLink(any_to_quit, any_flow, any_out, quit, quit_flow);
  init_to_any.multi_input_sort_id = 2;
  update_to_any_first.multi_input_sort_id = 1;
  update_to_any_second.multi_input_sort_id = 0;
  Append(tree.links, update_to_any_first);
  Append(tree.links, any_to_quit);
  Append(tree.links, init_to_any);
  Append(tree.links, update_to_any_second);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  for (const LN_Event event : {LN_Event::OnInit, LN_Event::OnUpdate}) {
    const std::vector<LN_Instruction> &instructions = program->GetInstructions(event);
    const int expected_pulses = event == LN_Event::OnInit ? 1 : 2;
    int route_count = 0;
    int quit_count = 0;
    std::vector<uint32_t> guarded_route_indices;
    for (const LN_Instruction &instruction : instructions) {
      route_count += instruction.opcode == LN_OpCode::BranchRoute ? 1 : 0;
      if (instruction.opcode != LN_OpCode::QuitGame) {
        continue;
      }
      quit_count++;
      const LN_BoolExpression *guard = BoolExpressionAt(*program,
                                                        instruction.bool_guard_expr_index);
      ASSERT_NE(guard, nullptr);
      ASSERT_EQ(guard->kind, LN_BoolExpressionKind::InstructionExecuted);
      ASSERT_LT(guard->input0, instructions.size());
      EXPECT_EQ(instructions[guard->input0].opcode, LN_OpCode::BranchRoute);
      guarded_route_indices.push_back(guard->input0);
    }
    EXPECT_EQ(route_count, expected_pulses);
    EXPECT_EQ(quit_count, expected_pulses);
    std::sort(guarded_route_indices.begin(), guarded_route_indices.end());
    EXPECT_EQ(std::adjacent_find(guarded_route_indices.begin(), guarded_route_indices.end()),
              guarded_route_indices.end());
  }
}

TEST(LN_TreeCompiler, SequenceCompilesOutputsInNumericOrderIndependentOfLinkOrder)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket update_out = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(update_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, update_out);
  Append(tree.nodes, on_update);

  blender::bNode sequence = {};
  blender::bNodeSocket sequence_flow = {};
  std::array<blender::bNodeSocket, 6> then_outputs = {};
  InitNode(sequence, "LogicNativeSequence", "Sequence", 2);
  InitSocket(sequence_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  Append(sequence.inputs, sequence_flow);
  for (int index = 0; index < int(then_outputs.size()); index++) {
    const std::string name = "Then " + std::to_string(index);
    InitSocket(then_outputs[size_t(index)], name.c_str(), blender::SOCK_BOOLEAN, nullptr);
    Append(sequence.outputs, then_outputs[size_t(index)]);
  }
  Append(tree.nodes, sequence);

  blender::bNode restart = {};
  blender::bNodeSocket restart_flow = {};
  InitNode(restart, "LogicNativeRestartGame", "Then 1 Command", 3);
  InitSocket(restart_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  Append(restart.inputs, restart_flow);
  Append(tree.nodes, restart);

  blender::bNode quit = {};
  blender::bNodeSocket quit_flow = {};
  InitNode(quit, "LogicNativeQuitGame", "Then 0 Command", 4);
  InitSocket(quit_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  Append(quit.inputs, quit_flow);
  Append(tree.nodes, quit);

  blender::bNode then_two = {};
  blender::bNodeSocket then_two_flow = {};
  InitNode(then_two, "LogicNativeQuitGame", "Then 2 Command", 5);
  InitSocket(then_two_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  Append(then_two.inputs, then_two_flow);
  Append(tree.nodes, then_two);

  blender::bNode then_three = {};
  blender::bNodeSocket then_three_flow = {};
  InitNode(then_three, "LogicNativeQuitGame", "Then 3 Command", 6);
  InitSocket(then_three_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  Append(then_three.inputs, then_three_flow);
  Append(tree.nodes, then_three);

  blender::bNode then_four = {};
  blender::bNodeSocket then_four_flow = {};
  InitNode(then_four, "LogicNativeQuitGame", "Then 4 Command", 7);
  InitSocket(then_four_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  Append(then_four.inputs, then_four_flow);
  Append(tree.nodes, then_four);

  blender::bNode then_five = {};
  blender::bNodeSocket then_five_flow = {};
  InitNode(then_five, "LogicNativeQuitGame", "Then 5 Command", 8);
  InitSocket(then_five_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  Append(then_five.inputs, then_five_flow);
  Append(tree.nodes, then_five);

  blender::bNodeLink event_to_sequence = {};
  blender::bNodeLink then_one_to_restart = {};
  blender::bNodeLink then_zero_to_quit = {};
  blender::bNodeLink then_two_to_command = {};
  blender::bNodeLink then_three_to_command = {};
  blender::bNodeLink then_four_to_command = {};
  blender::bNodeLink then_five_to_command = {};
  InitLink(event_to_sequence, on_update, update_out, sequence, sequence_flow);
  InitLink(then_one_to_restart, sequence, then_outputs[1], restart, restart_flow);
  InitLink(then_zero_to_quit, sequence, then_outputs[0], quit, quit_flow);
  InitLink(then_two_to_command, sequence, then_outputs[2], then_two, then_two_flow);
  InitLink(then_three_to_command, sequence, then_outputs[3], then_three, then_three_flow);
  InitLink(then_four_to_command, sequence, then_outputs[4], then_four, then_four_flow);
  InitLink(then_five_to_command, sequence, then_outputs[5], then_five, then_five_flow);
  Append(tree.links, then_five_to_command);
  Append(tree.links, then_three_to_command);
  Append(tree.links, then_one_to_restart);
  Append(tree.links, event_to_sequence);
  Append(tree.links, then_four_to_command);
  Append(tree.links, then_two_to_command);
  Append(tree.links, then_zero_to_quit);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  const std::vector<LN_Instruction> &instructions = program->GetInstructions(LN_Event::OnUpdate);
  std::vector<int32_t> command_node_order;
  for (const LN_Instruction &instruction : instructions) {
    if ((instruction.opcode != LN_OpCode::QuitGame &&
         instruction.opcode != LN_OpCode::RestartGame) ||
        instruction.source_ref_index >= program->GetSourceRefs().size())
    {
      continue;
    }
    command_node_order.push_back(
        program->GetSourceRefs()[instruction.source_ref_index].source_node_identifier);
  }
  EXPECT_EQ(command_node_order, (std::vector<int32_t>{4, 3, 5, 6, 7, 8}));

  std::vector<std::string> sequence_routes;
  for (const LN_Instruction &instruction : instructions) {
    if (instruction.opcode != LN_OpCode::BranchRoute ||
        instruction.source_ref_index >= program->GetSourceRefs().size())
    {
      continue;
    }
    const LN_SourceRef &source = program->GetSourceRefs()[instruction.source_ref_index];
    if (source.node_idname == "LogicNativeSequence") {
      sequence_routes.push_back(source.socket_name);
    }
  }
  ASSERT_EQ(sequence_routes.size(), 6u);
  EXPECT_EQ(sequence_routes[0], "Then 0");
  EXPECT_EQ(sequence_routes[1], "Then 1");
  EXPECT_EQ(sequence_routes[2], "Then 2");
  EXPECT_EQ(sequence_routes[3], "Then 3");
  EXPECT_EQ(sequence_routes[4], "Then 4");
  EXPECT_EQ(sequence_routes[5], "Then 5");
}

TEST(LN_TreeCompiler, SequenceRejectsSparseDynamicOutputs)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket update_out = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(update_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, update_out);
  Append(tree.nodes, on_update);

  blender::bNode sequence = {};
  blender::bNodeSocket sequence_flow = {};
  blender::bNodeSocket then_zero = {};
  blender::bNodeSocket then_two = {};
  InitNode(sequence, "LogicNativeSequence", "Sequence", 2);
  InitSocket(sequence_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(then_zero, "Then 0", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(then_two, "Then 2", blender::SOCK_BOOLEAN, nullptr);
  Append(sequence.inputs, sequence_flow);
  Append(sequence.outputs, then_zero);
  Append(sequence.outputs, then_two);
  Append(tree.nodes, sequence);

  blender::bNode quit = {};
  blender::bNodeSocket quit_flow = {};
  InitNode(quit, "LogicNativeQuitGame", "Quit", 3);
  InitSocket(quit_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  Append(quit.inputs, quit_flow);
  Append(tree.nodes, quit);

  blender::bNodeLink update_to_sequence = {};
  blender::bNodeLink sequence_to_quit = {};
  InitLink(update_to_sequence, on_update, update_out, sequence, sequence_flow);
  InitLink(sequence_to_quit, sequence, then_zero, quit, quit_flow);
  Append(tree.links, update_to_sequence);
  Append(tree.links, sequence_to_quit);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  const LN_CompileReport &report = program->GetCompileReport();
  EXPECT_TRUE(report.HasErrors()) << FormatCompileReport(report);
  EXPECT_TRUE(HasErrorContaining(report, "numbered consecutively from Then 0"))
      << FormatCompileReport(report);
}

TEST(LN_TreeCompiler, FlowControlNodesCompileToDenseNativeStateInstructions)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket update_out = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(update_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, update_out);
  Append(tree.nodes, on_update);

  blender::bNode router = {};
  blender::bNodeSocket router_flow = {};
  blender::bNodeSocket router_reset = {};
  blender::bNodeSocket router_then_zero = {};
  blender::bNodeSocket router_then_one = {};
  InitNode(router, "LogicNativeFlowRouter", "Flow Router", 2);
  InitSocket(router_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(router_reset, "Reset", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(router_then_zero, "Then 0", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(router_then_one, "Then 1", blender::SOCK_BOOLEAN, nullptr);
  Append(router.inputs, router_flow);
  Append(router.inputs, router_reset);
  Append(router.outputs, router_then_zero);
  Append(router.outputs, router_then_one);
  Append(tree.nodes, router);

  blender::bNode flip_flop = {};
  blender::bNodeSocket flip_flop_flow = {};
  blender::bNodeSocket flip_flop_reset = {};
  blender::bNodeSocket flip_flop_a = {};
  blender::bNodeSocket flip_flop_b = {};
  blender::bNodeSocket flip_flop_is_a = {};
  InitNode(flip_flop, "LogicNativeFlipFlop", "Flip-Flop", 3);
  InitSocket(flip_flop_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(flip_flop_reset, "Reset", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(flip_flop_a, "A", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(flip_flop_b, "B", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(flip_flop_is_a, "Is A", blender::SOCK_BOOLEAN, nullptr);
  Append(flip_flop.inputs, flip_flop_flow);
  Append(flip_flop.inputs, flip_flop_reset);
  Append(flip_flop.outputs, flip_flop_a);
  Append(flip_flop.outputs, flip_flop_b);
  Append(flip_flop.outputs, flip_flop_is_a);
  Append(tree.nodes, flip_flop);

  blender::bNodeSocketValueInt limit_default = {};
  limit_default.value = 2;
  blender::bNode repeat_limit = {};
  blender::bNodeSocket repeat_flow = {};
  blender::bNodeSocket repeat_reset = {};
  blender::bNodeSocket repeat_limit_value = {};
  blender::bNodeSocket repeat_accepted = {};
  blender::bNodeSocket repeat_blocked = {};
  blender::bNodeSocket repeat_remaining = {};
  blender::bNodeSocket repeat_exhausted = {};
  InitNode(repeat_limit, "LogicNativeRepeatLimit", "Repeat Limit", 4);
  InitSocket(repeat_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(repeat_reset, "Reset", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(repeat_limit_value, "Limit", blender::SOCK_INT, &limit_default);
  InitSocket(repeat_accepted, "Accepted", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(repeat_blocked, "Blocked", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(repeat_remaining, "Remaining", blender::SOCK_INT, nullptr);
  InitSocket(repeat_exhausted, "Is Exhausted", blender::SOCK_BOOLEAN, nullptr);
  Append(repeat_limit.inputs, repeat_flow);
  Append(repeat_limit.inputs, repeat_reset);
  Append(repeat_limit.inputs, repeat_limit_value);
  Append(repeat_limit.outputs, repeat_accepted);
  Append(repeat_limit.outputs, repeat_blocked);
  Append(repeat_limit.outputs, repeat_remaining);
  Append(repeat_limit.outputs, repeat_exhausted);
  Append(tree.nodes, repeat_limit);

  std::array<blender::bNode, 3> quit_nodes = {};
  std::array<blender::bNodeSocket, 3> quit_flows = {};
  for (int index = 0; index < int(quit_nodes.size()); index++) {
    InitNode(quit_nodes[size_t(index)], "LogicNativeQuitGame", "Quit", 5 + index);
    InitSocket(quit_flows[size_t(index)], "Flow", blender::SOCK_BOOLEAN, nullptr);
    Append(quit_nodes[size_t(index)].inputs, quit_flows[size_t(index)]);
    Append(tree.nodes, quit_nodes[size_t(index)]);
  }

  std::array<blender::bNodeLink, 6> links = {};
  InitLink(links[0], on_update, update_out, router, router_flow);
  InitLink(links[1], on_update, update_out, flip_flop, flip_flop_flow);
  InitLink(links[2], on_update, update_out, repeat_limit, repeat_flow);
  InitLink(links[3], router, router_then_zero, quit_nodes[0], quit_flows[0]);
  InitLink(links[4], flip_flop, flip_flop_a, quit_nodes[1], quit_flows[1]);
  InitLink(links[5], repeat_limit, repeat_accepted, quit_nodes[2], quit_flows[2]);
  for (blender::bNodeLink &link : links) {
    Append(tree.links, link);
  }

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  EXPECT_EQ(program->GetFlowControlStateCount(), 3u);

  const std::vector<LN_Instruction> &instructions = program->GetInstructions(LN_Event::OnUpdate);
  std::vector<const LN_Instruction *> router_attempts;
  for (const LN_Instruction &instruction : instructions) {
    if (instruction.opcode == LN_OpCode::TryFlowRouter) {
      router_attempts.push_back(&instruction);
    }
  }
  const LN_Instruction *repeat_attempt = FindInstruction(instructions, LN_OpCode::TryRepeatLimit);
  ASSERT_EQ(router_attempts.size(), 2u);
  ASSERT_NE(repeat_attempt, nullptr);
  EXPECT_EQ(router_attempts[0]->secondary_int_value, 2);
  EXPECT_EQ(router_attempts[1]->secondary_int_value, 2);
  EXPECT_NE(router_attempts[0]->int_value, router_attempts[1]->int_value);
  EXPECT_NE(repeat_attempt->int_value, router_attempts[0]->int_value);
  EXPECT_NE(repeat_attempt->int_value, router_attempts[1]->int_value);

  EXPECT_TRUE(std::any_of(program->GetBoolExpressions().begin(),
                          program->GetBoolExpressions().end(),
                          [](const LN_BoolExpression &expression) {
                            return expression.kind == LN_BoolExpressionKind::FlowRouterSelected;
                          }));
  EXPECT_TRUE(std::any_of(program->GetBoolExpressions().begin(),
                          program->GetBoolExpressions().end(),
                          [](const LN_BoolExpression &expression) {
                            return expression.kind == LN_BoolExpressionKind::RepeatLimitAccepted;
                          }));
  EXPECT_TRUE(std::any_of(program->GetIntExpressions().begin(),
                          program->GetIntExpressions().end(),
                          [](const LN_IntExpression &expression) {
                            return expression.kind == LN_IntExpressionKind::RepeatLimitRemaining;
                          }));
}

TEST(LN_TreeCompiler, SwitchFlowAndLoopCompletionCompileWithExplicitBreak)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket update_out = {};
  InitNode(on_update, "LogicNativeOnFixedUpdate", "On Fixed Update", 1);
  InitSocket(update_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, update_out);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueInt index_default = {};
  index_default.value = 1;
  blender::bNode int_switch = {};
  blender::bNodeSocket int_switch_flow = {};
  blender::bNodeSocket int_switch_index = {};
  blender::bNodeSocket int_then_zero = {};
  blender::bNodeSocket int_then_one = {};
  blender::bNodeSocket int_default = {};
  InitNode(int_switch, "LogicNativeSwitchFlowInt", "Integer Switch", 2);
  InitSocket(int_switch_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(int_switch_index, "Index", blender::SOCK_INT, &index_default);
  InitSocket(int_then_zero, "Then 0", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(int_then_one, "Then 1", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(int_default, "Default", blender::SOCK_BOOLEAN, nullptr);
  Append(int_switch.inputs, int_switch_flow);
  Append(int_switch.inputs, int_switch_index);
  Append(int_switch.outputs, int_then_zero);
  Append(int_switch.outputs, int_then_one);
  Append(int_switch.outputs, int_default);
  Append(tree.nodes, int_switch);

  blender::bNodeSocketValueString string_value_default = {};
  blender::bNodeSocketValueString case_zero_default = {};
  blender::bNodeSocketValueString case_one_default = {};
  SetCString(string_value_default.value, "blue");
  SetCString(case_zero_default.value, "red");
  SetCString(case_one_default.value, "blue");
  blender::bNode string_switch = {};
  blender::bNodeSocket string_switch_flow = {};
  blender::bNodeSocket string_switch_value = {};
  blender::bNodeSocket string_case_zero = {};
  blender::bNodeSocket string_case_one = {};
  blender::bNodeSocket string_then_zero = {};
  blender::bNodeSocket string_then_one = {};
  blender::bNodeSocket string_default = {};
  InitNode(string_switch, "LogicNativeSwitchFlowString", "String Switch", 3);
  InitSocket(string_switch_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(string_switch_value, "Value", blender::SOCK_STRING, &string_value_default);
  InitSocket(string_case_zero, "Case 0", blender::SOCK_STRING, &case_zero_default);
  InitSocket(string_case_one, "Case 1", blender::SOCK_STRING, &case_one_default);
  InitSocket(string_then_zero, "Then 0", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(string_then_one, "Then 1", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(string_default, "Default", blender::SOCK_BOOLEAN, nullptr);
  Append(string_switch.inputs, string_switch_flow);
  Append(string_switch.inputs, string_switch_value);
  Append(string_switch.inputs, string_case_zero);
  Append(string_switch.inputs, string_case_one);
  Append(string_switch.outputs, string_then_zero);
  Append(string_switch.outputs, string_then_one);
  Append(string_switch.outputs, string_default);
  Append(tree.nodes, string_switch);

  blender::bNodeSocketValueInt count_default = {};
  count_default.value = 2;
  blender::bNodeSocketValueInt ticks_default = {};
  ticks_default.value = 1;
  blender::bNode loop = {};
  blender::bNodeSocket loop_flow = {};
  blender::bNodeSocket loop_count = {};
  blender::bNodeSocket loop_ticks = {};
  blender::bNodeSocket loop_out = {};
  blender::bNodeSocket loop_completed = {};
  blender::bNodeSocket loop_index = {};
  InitNode(loop, "LogicNativeLoop", "Loop", 4);
  InitSocket(loop_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(loop_count, "Count", blender::SOCK_INT, &count_default);
  InitSocket(loop_ticks, "Ticks to Complete", blender::SOCK_INT, &ticks_default);
  InitSocket(loop_out, "Loop", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(loop_completed, "Completed", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(loop_index, "Index", blender::SOCK_INT, nullptr);
  Append(loop.inputs, loop_flow);
  Append(loop.inputs, loop_count);
  Append(loop.inputs, loop_ticks);
  Append(loop.outputs, loop_out);
  Append(loop.outputs, loop_completed);
  Append(loop.outputs, loop_index);
  Append(tree.nodes, loop);

  blender::bNode break_loop = {};
  blender::bNodeSocket break_flow = {};
  InitNode(break_loop, "LogicNativeBreakLoop", "Break Loop", 5);
  InitSocket(break_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  Append(break_loop.inputs, break_flow);
  Append(tree.nodes, break_loop);

  std::array<blender::bNode, 7> quit_nodes = {};
  std::array<blender::bNodeSocket, 7> quit_flows = {};
  for (int index = 0; index < int(quit_nodes.size()); index++) {
    InitNode(quit_nodes[size_t(index)], "LogicNativeQuitGame", "Quit", 6 + index);
    InitSocket(quit_flows[size_t(index)], "Flow", blender::SOCK_BOOLEAN, nullptr);
    Append(quit_nodes[size_t(index)].inputs, quit_flows[size_t(index)]);
    Append(tree.nodes, quit_nodes[size_t(index)]);
  }

  std::array<blender::bNodeLink, 11> links = {};
  InitLink(links[0], on_update, update_out, int_switch, int_switch_flow);
  InitLink(links[1], on_update, update_out, string_switch, string_switch_flow);
  InitLink(links[2], on_update, update_out, loop, loop_flow);
  InitLink(links[3], int_switch, int_then_zero, quit_nodes[0], quit_flows[0]);
  InitLink(links[4], int_switch, int_then_one, quit_nodes[1], quit_flows[1]);
  InitLink(links[5], int_switch, int_default, quit_nodes[2], quit_flows[2]);
  InitLink(links[6], string_switch, string_then_zero, quit_nodes[3], quit_flows[3]);
  InitLink(links[7], string_switch, string_then_one, quit_nodes[4], quit_flows[4]);
  InitLink(links[8], string_switch, string_default, quit_nodes[5], quit_flows[5]);
  InitLink(links[9], loop, loop_out, break_loop, break_flow);
  InitLink(links[10], loop, loop_completed, quit_nodes[6], quit_flows[6]);
  for (blender::bNodeLink &link : links) {
    Append(tree.links, link);
  }

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  ASSERT_EQ(program->GetLoopFrames().size(), 1u);

  const std::vector<LN_Instruction> &instructions = program->GetInstructions(
      LN_Event::OnFixedUpdate);
  const LN_Instruction *break_instruction = FindInstruction(instructions, LN_OpCode::BreakLoop);
  ASSERT_NE(break_instruction, nullptr);
  EXPECT_EQ(break_instruction->loop_frame_index, 0u);
  EXPECT_EQ(break_instruction->int_value, 0);

  const LN_Instruction *completed_quit = nullptr;
  for (const LN_Instruction &instruction : instructions) {
    if (instruction.opcode == LN_OpCode::QuitGame &&
        instruction.completed_loop_frame_index != LN_INVALID_INDEX)
    {
      completed_quit = &instruction;
      break;
    }
  }
  ASSERT_NE(completed_quit, nullptr);
  EXPECT_EQ(completed_quit->completed_loop_frame_index, 0u);
  EXPECT_TRUE(std::any_of(program->GetBoolExpressions().begin(),
                          program->GetBoolExpressions().end(),
                          [](const LN_BoolExpression &expression) {
                            return expression.kind == LN_BoolExpressionKind::ValueCompare;
                          }));

  size_t switch_comparison_count = 0;
  bool found_integer_one_selector = false;
  bool found_string_blue_selector = false;
  for (const LN_BoolExpression &expression : program->GetBoolExpressions()) {
    if (expression.kind != LN_BoolExpressionKind::ValueCompare) {
      continue;
    }
    switch_comparison_count++;
    const LN_ValueExpression *selector = ValueExpressionAt(*program, expression.input0);
    const LN_ValueExpression *case_value = ValueExpressionAt(*program, expression.input1);
    ASSERT_NE(selector, nullptr);
    ASSERT_NE(case_value, nullptr);
    ASSERT_EQ(selector->kind, LN_ValueExpressionKind::Constant);
    ASSERT_EQ(case_value->kind, LN_ValueExpressionKind::Constant);
    EXPECT_TRUE(selector->value.exists);
    EXPECT_TRUE(case_value->value.exists);
    EXPECT_EQ(selector->value.type, case_value->value.type);
    if (selector->value.type == LN_ValueType::Int && selector->value.int_value == 1) {
      found_integer_one_selector = true;
    }
    if (selector->value.type == LN_ValueType::String && selector->value.string_value == "blue") {
      found_string_blue_selector = true;
    }
  }
  EXPECT_EQ(switch_comparison_count, 4u);
  EXPECT_TRUE(found_integer_one_selector);
  EXPECT_TRUE(found_string_blue_selector);

  size_t switch_default_route_count = 0;
  for (const LN_Instruction &instruction : instructions) {
    if (instruction.opcode != LN_OpCode::BranchRoute ||
        instruction.source_ref_index >= program->GetSourceRefs().size())
    {
      continue;
    }
    const LN_SourceRef &source = program->GetSourceRefs()[instruction.source_ref_index];
    if ((source.node_idname == "LogicNativeSwitchFlowInt" ||
         source.node_idname == "LogicNativeSwitchFlowString") &&
        source.socket_name == "Default")
    {
      switch_default_route_count++;
    }
  }
  EXPECT_EQ(switch_default_route_count, 2u);
}

TEST(LN_TreeCompiler, CompletedOnlyLoopBuildsDriverBodyBeforeCompletion)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket update_out = {};
  InitNode(on_update, "LogicNativeOnFixedUpdate", "On Fixed Update", 1);
  InitSocket(update_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, update_out);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueInt count_default = {};
  count_default.value = 0;
  blender::bNodeSocketValueInt ticks_default = {};
  ticks_default.value = 3;
  blender::bNode loop = {};
  blender::bNodeSocket loop_flow = {};
  blender::bNodeSocket loop_count = {};
  blender::bNodeSocket loop_ticks = {};
  blender::bNodeSocket loop_completed = {};
  InitNode(loop, "LogicNativeLoop", "Loop", 2);
  InitSocket(loop_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(loop_count, "Count", blender::SOCK_INT, &count_default);
  InitSocket(loop_ticks, "Ticks to Complete", blender::SOCK_INT, &ticks_default);
  InitSocket(loop_completed, "Completed", blender::SOCK_BOOLEAN, nullptr);
  Append(loop.inputs, loop_flow);
  Append(loop.inputs, loop_count);
  Append(loop.inputs, loop_ticks);
  Append(loop.outputs, loop_completed);
  Append(tree.nodes, loop);

  blender::bNode quit = {};
  blender::bNodeSocket quit_flow = {};
  InitNode(quit, "LogicNativeQuitGame", "Quit", 3);
  InitSocket(quit_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  Append(quit.inputs, quit_flow);
  Append(tree.nodes, quit);

  blender::bNodeLink update_to_loop = {};
  blender::bNodeLink completed_to_quit = {};
  InitLink(update_to_loop, on_update, update_out, loop, loop_flow);
  InitLink(completed_to_quit, loop, loop_completed, quit, quit_flow);
  Append(tree.links, update_to_loop);
  Append(tree.links, completed_to_quit);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  ASSERT_EQ(program->GetLoopFrames().size(), 1u);
  const LN_LoopFrame &frame = program->GetLoopFrames().front();
  const LN_IntExpression *ticks_expression = IntExpressionAt(
      *program, frame.ticks_to_complete_int_expr_index);
  ASSERT_NE(ticks_expression, nullptr);
  EXPECT_EQ(ticks_expression->kind, LN_IntExpressionKind::Constant);
  EXPECT_EQ(ticks_expression->int_value, 3);

  const std::vector<LN_Instruction> &instructions = program->GetInstructions(
      LN_Event::OnFixedUpdate);
  const LN_Instruction *driver = nullptr;
  const LN_Instruction *completed_quit = nullptr;
  for (const LN_Instruction &instruction : instructions) {
    if (instruction.opcode == LN_OpCode::Nop && instruction.loop_frame_index == 0u &&
        instruction.completed_loop_frame_index == LN_INVALID_INDEX)
    {
      driver = &instruction;
    }
    if (instruction.opcode == LN_OpCode::QuitGame && instruction.completed_loop_frame_index == 0u)
    {
      completed_quit = &instruction;
    }
  }
  ASSERT_NE(driver, nullptr);
  ASSERT_NE(completed_quit, nullptr);
}

/** A Count Loop that quits when completed, triggered by the given events through Any Flow. */
std::shared_ptr<LN_Program> CompileQuitLoop(const std::vector<const char *> &event_idnames)
{
  blender::bNodeTree tree = {};
  InitTree(tree);
  std::vector<blender::bNode> events(event_idnames.size());
  std::vector<blender::bNodeSocket> event_outs(event_idnames.size());
  for (size_t index = 0; index < events.size(); ++index) {
    InitNode(events[index], event_idnames[index], event_idnames[index], int(10 + index));
    InitSocket(event_outs[index], "Out", blender::SOCK_BOOLEAN, nullptr);
    Append(events[index].outputs, event_outs[index]);
    Append(tree.nodes, events[index]);
  }

  blender::bNodeSocketValueInt count_default = {};
  count_default.value = 4;
  blender::bNodeSocketValueInt ticks_default = {};
  ticks_default.value = 2;
  blender::bNode loop = {};
  blender::bNodeSocket loop_flow = {};
  blender::bNodeSocket loop_count = {};
  blender::bNodeSocket loop_ticks = {};
  blender::bNodeSocket loop_completed = {};
  InitNode(loop, "LogicNativeLoop", "Loop", 2);
  InitSocket(loop_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(loop_count, "Count", blender::SOCK_INT, &count_default);
  InitSocket(loop_ticks, "Ticks to Complete", blender::SOCK_INT, &ticks_default);
  InitSocket(loop_completed, "Completed", blender::SOCK_BOOLEAN, nullptr);
  Append(loop.inputs, loop_flow);
  Append(loop.inputs, loop_count);
  Append(loop.inputs, loop_ticks);
  Append(loop.outputs, loop_completed);
  Append(tree.nodes, loop);

  blender::bNode quit = {};
  blender::bNodeSocket quit_flow = {};
  InitNode(quit, "LogicNativeQuitGame", "Quit", 3);
  InitSocket(quit_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  Append(quit.inputs, quit_flow);
  Append(tree.nodes, quit);

  blender::bNode any_flow = {};
  blender::bNodeSocket any_in = {};
  blender::bNodeSocket any_out = {};
  InitNode(any_flow, "LogicNativeAnyFlow", "Any Flow", 4);
  InitSocket(any_in, "In", blender::SOCK_BOOLEAN, nullptr);
  any_in.flag |= blender::SOCK_MULTI_INPUT;
  InitSocket(any_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(any_flow.inputs, any_in);
  Append(any_flow.outputs, any_out);
  Append(tree.nodes, any_flow);

  std::vector<blender::bNodeLink> links(events.size() + 2);
  for (size_t index = 0; index < events.size(); ++index) {
    InitLink(links[index], events[index], event_outs[index], any_flow, any_in);
  }
  InitLink(links[events.size()], any_flow, any_out, loop, loop_flow);
  InitLink(links.back(), loop, loop_completed, quit, quit_flow);
  for (blender::bNodeLink &link : links) {
    Append(tree.links, link);
  }
  return LN_TreeCompiler().Compile(tree);
}

TEST(LN_TreeCompiler, LoopRunsOnFrameUpdate)
{
  const std::shared_ptr<LN_Program> program = CompileQuitLoop({"LogicNativeOnUpdate"});
  ASSERT_NE(program, nullptr);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  ASSERT_EQ(program->GetLoopFrames().size(), 1u);
  const std::vector<LN_Instruction> &instructions = program->GetInstructions(LN_Event::OnUpdate);
  EXPECT_TRUE(std::any_of(instructions.begin(), instructions.end(), [](const LN_Instruction &i) {
    return i.opcode == LN_OpCode::QuitGame && i.completed_loop_frame_index == 0u;
  }));
  EXPECT_TRUE(program->GetInstructions(LN_Event::OnFixedUpdate).empty());
}

TEST(LN_TreeCompiler, LoopFedByBothClocksIsRejected)
{
  /* One loop has one tick counter, so Frame Update and Logic + Physics ticks would both advance
   * it. */
  const std::shared_ptr<LN_Program> program = CompileQuitLoop(
      {"LogicNativeOnUpdate", "LogicNativeOnFixedUpdate"});
  ASSERT_NE(program, nullptr);
  EXPECT_TRUE(HasErrorContaining(program->GetCompileReport(), "cannot mix Frame Update"))
      << FormatCompileReport(program->GetCompileReport());
}

TEST(LN_TreeCompiler, InventoryNodeCompilesItsOperationAndInputs)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_init = {};
  blender::bNodeSocket on_init_pulse = {};
  InitNode(on_init, "LogicNativeOnInit", "On Init", 1);
  InitSocket(on_init_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_init.outputs, on_init_pulse);
  Append(tree.nodes, on_init);

  blender::bNodeSocketValueString item_default = {};
  SetCString(item_default.value, "apple");
  blender::bNodeSocketValueInt amount_default = {};
  amount_default.value = 5;
  blender::bNodeSocketValueInt max_stack_default = {};
  max_stack_default.value = 4;
  blender::bNodeSocketValueBoolean stack_default = {};
  stack_default.value = true;
  blender::bNodeSocketValueInt zero_int = {};
  blender::bNodeSocketValueBoolean false_default = {};
  blender::bNodeSocketValueString empty_string = {};
  blender::bNodeSocketValueCollection no_library = {};
  blender::bNodeSocketValueString property_default = {};
  SetCString(property_default.value, "inventory");
  blender::bNode inventory = {};
  blender::bNodeSocketValueBoolean keep_places_default = {};
  /* Override on: the node's own Max Stack, Stack... instead of the list's Inventory Rules. */
  blender::bNodeSocketValueBoolean override_default = {};
  override_default.value = true;
  InitNode(inventory, "LogicNativeInventory", "Add Item", 2);
  inventory.custom1 = int16_t(LN_InventoryOperation::AddItem);
  const std::array<const char *, 20> input_names = {"Flow",
                                                    "Object",
                                                    "Property",
                                                    "Item",
                                                    "Amount",
                                                    "Max Stack",
                                                    "Stack",
                                                    "Capacity",
                                                    "Slot",
                                                    "Other Slot",
                                                    "By Count",
                                                    "Descending",
                                                    "Recipes",
                                                    "Recipe",
                                                    "Library",
                                                    "Swap",
                                                    "To Object",
                                                    "To Property",
                                                    "Keep Places",
                                                    "Override"};
  const std::array<blender::eNodeSocketDatatype, 20> input_types = {
      blender::SOCK_BOOLEAN, blender::SOCK_OBJECT,  blender::SOCK_STRING,     blender::SOCK_STRING,
      blender::SOCK_INT,     blender::SOCK_INT,     blender::SOCK_BOOLEAN,    blender::SOCK_INT,
      blender::SOCK_INT,     blender::SOCK_INT,     blender::SOCK_BOOLEAN,    blender::SOCK_BOOLEAN,
      blender::SOCK_CUSTOM,  blender::SOCK_STRING,  blender::SOCK_COLLECTION, blender::SOCK_BOOLEAN,
      blender::SOCK_OBJECT,  blender::SOCK_STRING,  blender::SOCK_BOOLEAN,    blender::SOCK_BOOLEAN};
  const std::array<void *, 20> input_defaults = {nullptr,
                                                 nullptr,
                                                 &property_default,
                                                 &item_default,
                                                 &amount_default,
                                                 &max_stack_default,
                                                 &stack_default,
                                                 &zero_int,
                                                 &zero_int,
                                                 &zero_int,
                                                 &false_default,
                                                 &false_default,
                                                 nullptr,
                                                 &empty_string,
                                                 &no_library,
                                                 &stack_default,
                                                 nullptr,
                                                 &empty_string,
                                                 &keep_places_default,
                                                 &override_default};
  std::array<blender::bNodeSocket, 20> inputs = {};
  for (size_t index = 0; index < inputs.size(); ++index) {
    InitSocket(inputs[index],
               input_names[index],
               input_types[index],
               input_defaults[index],
               index == 0 ? "NodeSocketLogicExecution" :
                            (index == 12 ? "NodeSocketLogicList" : nullptr));
    Append(inventory.inputs, inputs[index]);
  }
  blender::bNodeSocket done = {};
  blender::bNodeSocket inventory_out = {};
  blender::bNodeSocket leftover = {};
  InitSocket(done, "Done", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  InitSocketWithIdentifier(
      inventory_out, "Inventory", "Inventory", blender::SOCK_CUSTOM, nullptr, "NodeSocketLogicList");
  InitSocket(leftover, "Leftover", blender::SOCK_INT, nullptr);
  Append(inventory.outputs, done);
  Append(inventory.outputs, inventory_out);
  Append(inventory.outputs, leftover);
  Append(tree.nodes, inventory);

  blender::bNode print_node = {};
  blender::bNodeSocket print_flow = {};
  blender::bNodeSocket print_value = {};
  InitNode(print_node, "LogicNativePrint", "Print", 3);
  InitSocket(print_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(print_value, "Value", blender::SOCK_CUSTOM, nullptr);
  Append(print_node.inputs, print_flow);
  Append(print_node.inputs, print_value);
  Append(tree.nodes, print_node);

  std::array<blender::bNodeLink, 3> links = {};
  InitLink(links[0], on_init, on_init_pulse, inventory, inputs[0]);
  InitLink(links[1], inventory, done, print_node, print_flow);
  InitLink(links[2], inventory, leftover, print_node, print_value);
  for (blender::bNodeLink &link : links) {
    Append(tree.links, link);
  }

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(program, nullptr);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  /* The node is one instruction that edits the owner's "inventory" property in place. */
  const LN_Instruction *edit_instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnInit), LN_OpCode::InventoryEdit);
  ASSERT_NE(edit_instruction, nullptr);
  EXPECT_EQ(edit_instruction->value_expr_index, LN_INVALID_INDEX);
  EXPECT_NE(edit_instruction->string_expr_index, LN_INVALID_INDEX);
  EXPECT_NE(edit_instruction->property_ref_index, LN_INVALID_INDEX);
  const LN_ValueExpression *edit = ValueExpressionAt(*program,
                                                     edit_instruction->secondary_value_expr_index);
  ASSERT_NE(edit, nullptr);
  EXPECT_EQ(edit->kind, LN_ValueExpressionKind::InventoryEdit);
  EXPECT_EQ(edit->value.int_value, int32_t(LN_InventoryOperation::AddItem));
  ASSERT_EQ(edit->input_indices.size(), size_t(LN_INVENTORY_INPUT_COUNT));
  const LN_ValueExpression *max_stack = ValueExpressionAt(
      *program, edit->input_indices[LN_INVENTORY_INPUT_MAX_STACK]);
  ASSERT_NE(max_stack, nullptr);
  EXPECT_EQ(max_stack->value.int_value, 4);
  const LN_ValueExpression *item = ValueExpressionAt(
      *program, edit->input_indices[LN_INVENTORY_INPUT_ITEM]);
  ASSERT_NE(item, nullptr);
  EXPECT_EQ(item->value.string_value, "apple");
  /* No item library given: Add Item falls back to the scene's Item Library. */
  EXPECT_EQ(edit->input_indices[LN_INVENTORY_INPUT_LIBRARY], LN_INVALID_INDEX);
  EXPECT_EQ(edit->input1, LN_INVENTORY_SCENE_LIBRARY);
  /* Override reaches the runtime, which then uses the node's own rules. */
  const LN_ValueExpression *override_value = ValueExpressionAt(
      *program, edit->input_indices[LN_INVENTORY_INPUT_OVERRIDE]);
  ASSERT_NE(override_value, nullptr);
  EXPECT_TRUE(override_value->value.bool_value);

  /* Done runs after it; Leftover reads what the instruction kept. */
  const LN_Instruction *print = FindInstruction(program->GetInstructions(LN_Event::OnInit),
                                                LN_OpCode::Print);
  ASSERT_NE(print, nullptr);
  EXPECT_LT(edit_instruction, print);
  const LN_ValueExpression *amount = ValueExpressionAt(*program, print->value_expr_index);
  ASSERT_NE(amount, nullptr);
  EXPECT_EQ(amount->kind, LN_ValueExpressionKind::InventoryAmount);
  EXPECT_EQ(amount->property_ref_index, edit_instruction->property_ref_index);
}

TEST(LN_TreeCompiler, SetTextWritesTheTextProperty)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_init = {};
  blender::bNodeSocket on_init_pulse = {};
  InitNode(on_init, "LogicNativeOnInit", "On Init", 1);
  InitSocket(on_init_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_init.outputs, on_init_pulse);
  Append(tree.nodes, on_init);

  blender::bNodeSocketValueString text_default = {};
  SetCString(text_default.value, "x3");
  blender::bNode set_text = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket object = {};
  blender::bNodeSocket text = {};
  InitNode(set_text, "LogicNativeSetText", "Set Text", 2);
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  InitSocket(object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(text, "Text", blender::SOCK_STRING, &text_default);
  Append(set_text.inputs, flow);
  Append(set_text.inputs, object);
  Append(set_text.inputs, text);
  Append(tree.nodes, set_text);

  blender::bNodeLink link = {};
  InitLink(link, on_init, on_init_pulse, set_text, flow);
  Append(tree.links, link);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(program, nullptr);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  const LN_Instruction *instruction = FindInstruction(program->GetInstructions(LN_Event::OnInit),
                                                      LN_OpCode::SetGameProperty);
  ASSERT_NE(instruction, nullptr);
  const LN_GamePropertyRef *property = GamePropertyRefAt(*program,
                                                         instruction->property_ref_index);
  ASSERT_NE(property, nullptr);
  EXPECT_EQ(property->name, "Text");
  EXPECT_EQ(instruction->property_value_type, LN_ValueType::String);
}

TEST(LN_TreeCompiler, ObjectPropertyNodesReadAndWriteAnyValue)
{
  /* Get Object Property "items" of another object, written by Set Object Property "copy". */
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_init = {};
  blender::bNodeSocket on_init_pulse = {};
  InitNode(on_init, "LogicNativeOnInit", "On Init", 1);
  InitSocket(on_init_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_init.outputs, on_init_pulse);
  Append(tree.nodes, on_init);

  blender::Object chest = {};
  SetCString(chest.id.name, "OBChest");
  blender::bNodeSocketValueObject chest_default = {};
  chest_default.value = &chest;
  blender::bNodeSocketValueString items_name = {};
  SetCString(items_name.value, "items");
  blender::bNode get = {};
  blender::bNodeSocket get_object = {};
  blender::bNodeSocket get_name = {};
  blender::bNodeSocket get_value = {};
  InitNode(get, "LogicNativeGetGameProperty", "Get Object Property", 2);
  InitSocket(get_object, "Object", blender::SOCK_OBJECT, &chest_default);
  InitSocket(get_name, "Property", blender::SOCK_STRING, &items_name);
  InitSocket(get_value, "Value", blender::SOCK_CUSTOM, nullptr, "NodeSocketLogicGeneric");
  Append(get.inputs, get_object);
  Append(get.inputs, get_name);
  Append(get.outputs, get_value);
  Append(tree.nodes, get);

  blender::bNodeSocketValueString copy_name = {};
  SetCString(copy_name.value, "copy");
  blender::bNode set = {};
  blender::bNodeSocket set_flow = {};
  blender::bNodeSocket set_object = {};
  blender::bNodeSocket set_name = {};
  blender::bNodeSocket set_value = {};
  InitNode(set, "LogicNativeSetGameProperty", "Set Object Property", 3);
  InitSocket(set_flow, "Flow", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  InitSocket(set_object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(set_name, "Property", blender::SOCK_STRING, &copy_name);
  InitSocket(set_value, "Value", blender::SOCK_CUSTOM, nullptr, "NodeSocketLogicGeneric");
  Append(set.inputs, set_flow);
  Append(set.inputs, set_object);
  Append(set.inputs, set_name);
  Append(set.inputs, set_value);
  Append(tree.nodes, set);

  std::array<blender::bNodeLink, 2> links = {};
  InitLink(links[0], on_init, on_init_pulse, set, set_flow);
  InitLink(links[1], get, get_value, set, set_value);
  for (blender::bNodeLink &link : links) {
    Append(tree.links, link);
  }

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(program, nullptr);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  const LN_Instruction *instruction = FindInstruction(program->GetInstructions(LN_Event::OnInit),
                                                      LN_OpCode::SetGameProperty);
  ASSERT_NE(instruction, nullptr);
  EXPECT_EQ(instruction->property_value_type, LN_ValueType::Generic);
  const LN_GamePropertyRef *property = GamePropertyRefAt(*program,
                                                         instruction->property_ref_index);
  ASSERT_NE(property, nullptr);
  EXPECT_EQ(property->name, "copy");
  ASSERT_LT(instruction->command_payload_index, program->GetGamePropertyCommandPayloads().size());
  const LN_GamePropertyCommandPayload &payload =
      program->GetGamePropertyCommandPayloads()[instruction->command_payload_index];
  /* No Object: the owner. The value is the live read of the chest's property. */
  EXPECT_EQ(payload.object_value_expr_index, LN_INVALID_INDEX);
  const LN_ValueExpression *value = ValueExpressionAt(*program, payload.value_expr_index);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(value->kind, LN_ValueExpressionKind::ObjectGameProperty);
  EXPECT_NE(value->input0, LN_INVALID_INDEX);
}

TEST(LN_TreeCompiler, LoopWithoutTicksToCompleteSocketIsRejectedWithoutLegacyFallback)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket update_out = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(update_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, update_out);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueInt count_default = {};
  count_default.value = 2;
  blender::bNode loop = {};
  blender::bNodeSocket loop_flow = {};
  blender::bNodeSocket loop_count = {};
  blender::bNodeSocket loop_body = {};
  InitNode(loop, "LogicNativeLoop", "Loop", 2);
  InitSocket(loop_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(loop_count, "Count", blender::SOCK_INT, &count_default);
  InitSocket(loop_body, "Loop", blender::SOCK_BOOLEAN, nullptr);
  Append(loop.inputs, loop_flow);
  Append(loop.inputs, loop_count);
  Append(loop.outputs, loop_body);
  Append(tree.nodes, loop);

  blender::bNode quit = {};
  blender::bNodeSocket quit_flow = {};
  InitNode(quit, "LogicNativeQuitGame", "Quit", 3);
  InitSocket(quit_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  Append(quit.inputs, quit_flow);
  Append(tree.nodes, quit);

  blender::bNodeLink update_to_loop = {};
  blender::bNodeLink loop_to_quit = {};
  InitLink(update_to_loop, on_update, update_out, loop, loop_flow);
  InitLink(loop_to_quit, loop, loop_body, quit, quit_flow);
  Append(tree.links, update_to_loop);
  Append(tree.links, loop_to_quit);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_TRUE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  EXPECT_TRUE(HasErrorContaining(program->GetCompileReport(), "Ticks to Complete"))
      << FormatCompileReport(program->GetCompileReport());
}

TEST(LN_TreeCompiler, OnInitLoopBuildsFixedContinuationRoutes)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_init = {};
  blender::bNodeSocket init_out = {};
  InitNode(on_init, "LogicNativeOnInit", "On Init", 1);
  InitSocket(init_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_init.outputs, init_out);
  Append(tree.nodes, on_init);

  blender::bNodeSocketValueInt count_default = {};
  count_default.value = 4;
  blender::bNodeSocketValueInt ticks_default = {};
  ticks_default.value = 2;
  blender::bNode loop = {};
  blender::bNodeSocket loop_flow = {};
  blender::bNodeSocket loop_count = {};
  blender::bNodeSocket loop_ticks = {};
  blender::bNodeSocket loop_body = {};
  blender::bNodeSocket loop_completed = {};
  InitNode(loop, "LogicNativeLoop", "Loop", 2);
  InitSocket(loop_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(loop_count, "Count", blender::SOCK_INT, &count_default);
  InitSocket(loop_ticks, "Ticks to Complete", blender::SOCK_INT, &ticks_default);
  InitSocket(loop_body, "Loop", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(loop_completed, "Completed", blender::SOCK_BOOLEAN, nullptr);
  Append(loop.inputs, loop_flow);
  Append(loop.inputs, loop_count);
  Append(loop.inputs, loop_ticks);
  Append(loop.outputs, loop_body);
  Append(loop.outputs, loop_completed);
  Append(tree.nodes, loop);

  blender::bNode body_quit = {};
  blender::bNodeSocket body_quit_flow = {};
  InitNode(body_quit, "LogicNativeQuitGame", "Body Quit", 3);
  InitSocket(body_quit_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  Append(body_quit.inputs, body_quit_flow);
  Append(tree.nodes, body_quit);

  blender::bNode completed_quit = {};
  blender::bNodeSocket completed_quit_flow = {};
  InitNode(completed_quit, "LogicNativeQuitGame", "Completed Quit", 4);
  InitSocket(completed_quit_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  Append(completed_quit.inputs, completed_quit_flow);
  Append(tree.nodes, completed_quit);

  blender::bNodeLink init_to_loop = {};
  blender::bNodeLink body_link = {};
  blender::bNodeLink completed_link = {};
  InitLink(init_to_loop, on_init, init_out, loop, loop_flow);
  InitLink(body_link, loop, loop_body, body_quit, body_quit_flow);
  InitLink(completed_link, loop, loop_completed, completed_quit, completed_quit_flow);
  Append(tree.links, init_to_loop);
  Append(tree.links, body_link);
  Append(tree.links, completed_link);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  ASSERT_EQ(program->GetLoopFrames().size(), 1u);

  auto has_loop_body = [&](const LN_Event event) {
    return std::any_of(program->GetInstructions(event).begin(),
                       program->GetInstructions(event).end(),
                       [](const LN_Instruction &instruction) {
                         return instruction.loop_frame_index == 0u;
                       });
  };
  auto has_loop_completion = [&](const LN_Event event) {
    return std::any_of(program->GetInstructions(event).begin(),
                       program->GetInstructions(event).end(),
                       [](const LN_Instruction &instruction) {
                         return instruction.completed_loop_frame_index == 0u;
                       });
  };
  EXPECT_TRUE(has_loop_body(LN_Event::OnInit));
  EXPECT_TRUE(has_loop_completion(LN_Event::OnInit));
  EXPECT_TRUE(has_loop_body(LN_Event::OnFixedUpdate));
  EXPECT_TRUE(has_loop_completion(LN_Event::OnFixedUpdate));
}

TEST(LN_TreeCompiler, FlowRouterRejectsSparseDynamicOutputs)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket update_out = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(update_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, update_out);
  Append(tree.nodes, on_update);

  blender::bNode router = {};
  blender::bNodeSocket router_flow = {};
  blender::bNodeSocket router_reset = {};
  blender::bNodeSocket router_then_zero = {};
  blender::bNodeSocket router_then_two = {};
  InitNode(router, "LogicNativeFlowRouter", "Flow Router", 2);
  InitSocket(router_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(router_reset, "Reset", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(router_then_zero, "Then 0", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(router_then_two, "Then 2", blender::SOCK_BOOLEAN, nullptr);
  Append(router.inputs, router_flow);
  Append(router.inputs, router_reset);
  Append(router.outputs, router_then_zero);
  Append(router.outputs, router_then_two);
  Append(tree.nodes, router);

  blender::bNode quit = {};
  blender::bNodeSocket quit_flow = {};
  InitNode(quit, "LogicNativeQuitGame", "Quit", 3);
  InitSocket(quit_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  Append(quit.inputs, quit_flow);
  Append(tree.nodes, quit);

  blender::bNodeLink update_to_router = {};
  blender::bNodeLink router_to_quit = {};
  InitLink(update_to_router, on_update, update_out, router, router_flow);
  InitLink(router_to_quit, router, router_then_zero, quit, quit_flow);
  Append(tree.links, update_to_router);
  Append(tree.links, router_to_quit);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  const LN_CompileReport &report = program->GetCompileReport();
  EXPECT_TRUE(report.HasErrors()) << FormatCompileReport(report);
  EXPECT_TRUE(HasErrorContaining(report, "numbered consecutively from Then 0"))
      << FormatCompileReport(report);
  EXPECT_EQ(
      FindInstruction(program->GetInstructions(LN_Event::OnUpdate), LN_OpCode::TryFlowRouter),
      nullptr);
}

TEST(LN_TreeCompiler, FlipFlopStateIsReadableBeforeItsFirstFlowPulse)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket update_out = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(update_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, update_out);
  Append(tree.nodes, on_update);

  blender::bNode flip_flop = {};
  blender::bNodeSocket flip_flow = {};
  blender::bNodeSocket flip_reset = {};
  blender::bNodeSocket flip_a = {};
  blender::bNodeSocket flip_b = {};
  blender::bNodeSocket flip_is_a = {};
  InitNode(flip_flop, "LogicNativeFlipFlop", "Flip-Flop", 2);
  InitSocket(flip_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(flip_reset, "Reset", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(flip_a, "A", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(flip_b, "B", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(flip_is_a, "Is A", blender::SOCK_BOOLEAN, nullptr);
  Append(flip_flop.inputs, flip_flow);
  Append(flip_flop.inputs, flip_reset);
  Append(flip_flop.outputs, flip_a);
  Append(flip_flop.outputs, flip_b);
  Append(flip_flop.outputs, flip_is_a);
  Append(tree.nodes, flip_flop);

  blender::bNodeSocketValueBoolean condition_default = {};
  blender::bNode branch = {};
  blender::bNodeSocket branch_flow = {};
  blender::bNodeSocket branch_condition = {};
  blender::bNodeSocket branch_true = {};
  blender::bNodeSocket branch_false = {};
  InitNode(branch, "LogicNativeBranch", "Branch", 3);
  InitSocket(branch_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(branch_condition, "Condition", blender::SOCK_BOOLEAN, &condition_default);
  InitSocket(branch_true, "True", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(branch_false, "False", blender::SOCK_BOOLEAN, nullptr);
  Append(branch.inputs, branch_flow);
  Append(branch.inputs, branch_condition);
  Append(branch.outputs, branch_true);
  Append(branch.outputs, branch_false);
  Append(tree.nodes, branch);

  blender::bNode quit = {};
  blender::bNodeSocket quit_flow = {};
  InitNode(quit, "LogicNativeQuitGame", "Quit", 4);
  InitSocket(quit_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  Append(quit.inputs, quit_flow);
  Append(tree.nodes, quit);

  std::array<blender::bNodeLink, 3> links = {};
  InitLink(links[0], on_update, update_out, branch, branch_flow);
  InitLink(links[1], flip_flop, flip_is_a, branch, branch_condition);
  InitLink(links[2], branch, branch_true, quit, quit_flow);
  for (blender::bNodeLink &link : links) {
    Append(tree.links, link);
  }

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  EXPECT_TRUE(std::any_of(program->GetBoolExpressions().begin(),
                          program->GetBoolExpressions().end(),
                          [](const LN_BoolExpression &expression) {
                            return expression.kind == LN_BoolExpressionKind::FlowRouterLastRoute;
                          }));
}

TEST(LN_TreeCompiler, FlipFlopRouteOutputsStillRequireFlowInput)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket update_out = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(update_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, update_out);
  Append(tree.nodes, on_update);

  blender::bNode flip_flop = {};
  blender::bNodeSocket flip_flow = {};
  blender::bNodeSocket flip_reset = {};
  blender::bNodeSocket flip_a = {};
  blender::bNodeSocket flip_b = {};
  blender::bNodeSocket flip_is_a = {};
  InitNode(flip_flop, "LogicNativeFlipFlop", "Flip-Flop", 2);
  InitSocket(flip_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(flip_reset, "Reset", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(flip_a, "A", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(flip_b, "B", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(flip_is_a, "Is A", blender::SOCK_BOOLEAN, nullptr);
  Append(flip_flop.inputs, flip_flow);
  Append(flip_flop.inputs, flip_reset);
  Append(flip_flop.outputs, flip_a);
  Append(flip_flop.outputs, flip_b);
  Append(flip_flop.outputs, flip_is_a);
  Append(tree.nodes, flip_flop);

  blender::bNode any_flow = {};
  blender::bNodeSocket any_in = {};
  blender::bNodeSocket any_out = {};
  InitNode(any_flow, "LogicNativeAnyFlow", "Any Flow", 3);
  InitSocket(any_in, "In", blender::SOCK_BOOLEAN, nullptr);
  any_in.flag |= blender::SOCK_MULTI_INPUT;
  InitSocket(any_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(any_flow.inputs, any_in);
  Append(any_flow.outputs, any_out);
  Append(tree.nodes, any_flow);

  blender::bNode quit = {};
  blender::bNodeSocket quit_flow = {};
  InitNode(quit, "LogicNativeQuitGame", "Quit", 4);
  InitSocket(quit_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  Append(quit.inputs, quit_flow);
  Append(tree.nodes, quit);

  std::array<blender::bNodeLink, 3> links = {};
  InitLink(links[0], on_update, update_out, any_flow, any_in);
  InitLink(links[1], flip_flop, flip_a, any_flow, any_in);
  InitLink(links[2], any_flow, any_out, quit, quit_flow);
  for (blender::bNodeLink &link : links) {
    Append(tree.links, link);
  }

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  const LN_CompileReport &report = program->GetCompileReport();
  EXPECT_TRUE(report.HasErrors()) << FormatCompileReport(report);
  EXPECT_TRUE(HasErrorContaining(report, "A and B outputs require an execution input"))
      << FormatCompileReport(report);
}

TEST(LN_TreeCompiler, RejectsNestedLoopBodiesWithActionableDiagnostic)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket update_out = {};
  InitNode(on_update, "LogicNativeOnFixedUpdate", "On Fixed Update", 1);
  InitSocket(update_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, update_out);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueInt outer_count_default = {};
  outer_count_default.value = 2;
  blender::bNodeSocketValueInt outer_ticks_default = {};
  outer_ticks_default.value = 1;
  blender::bNode outer_loop = {};
  blender::bNodeSocket outer_flow = {};
  blender::bNodeSocket outer_count = {};
  blender::bNodeSocket outer_ticks = {};
  blender::bNodeSocket outer_body = {};
  InitNode(outer_loop, "LogicNativeLoop", "Outer Loop", 2);
  InitSocket(outer_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(outer_count, "Count", blender::SOCK_INT, &outer_count_default);
  InitSocket(
      outer_ticks, "Ticks to Complete", blender::SOCK_INT, &outer_ticks_default);
  InitSocket(outer_body, "Loop", blender::SOCK_BOOLEAN, nullptr);
  Append(outer_loop.inputs, outer_flow);
  Append(outer_loop.inputs, outer_count);
  Append(outer_loop.inputs, outer_ticks);
  Append(outer_loop.outputs, outer_body);
  Append(tree.nodes, outer_loop);

  blender::bNodeSocketValueInt inner_count_default = {};
  inner_count_default.value = 2;
  blender::bNodeSocketValueInt inner_ticks_default = {};
  inner_ticks_default.value = 1;
  blender::bNode inner_loop = {};
  blender::bNodeSocket inner_flow = {};
  blender::bNodeSocket inner_count = {};
  blender::bNodeSocket inner_ticks = {};
  blender::bNodeSocket inner_body = {};
  InitNode(inner_loop, "LogicNativeLoop", "Inner Loop", 3);
  InitSocket(inner_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(inner_count, "Count", blender::SOCK_INT, &inner_count_default);
  InitSocket(
      inner_ticks, "Ticks to Complete", blender::SOCK_INT, &inner_ticks_default);
  InitSocket(inner_body, "Loop", blender::SOCK_BOOLEAN, nullptr);
  Append(inner_loop.inputs, inner_flow);
  Append(inner_loop.inputs, inner_count);
  Append(inner_loop.inputs, inner_ticks);
  Append(inner_loop.outputs, inner_body);
  Append(tree.nodes, inner_loop);

  blender::bNode quit = {};
  blender::bNodeSocket quit_flow = {};
  InitNode(quit, "LogicNativeQuitGame", "Quit", 4);
  InitSocket(quit_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  Append(quit.inputs, quit_flow);
  Append(tree.nodes, quit);

  std::array<blender::bNodeLink, 3> links = {};
  InitLink(links[0], on_update, update_out, outer_loop, outer_flow);
  InitLink(links[1], outer_loop, outer_body, inner_loop, inner_flow);
  InitLink(links[2], inner_loop, inner_body, quit, quit_flow);
  for (blender::bNodeLink &link : links) {
    Append(tree.links, link);
  }

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  const LN_CompileReport &report = program->GetCompileReport();
  EXPECT_TRUE(report.HasErrors()) << FormatCompileReport(report);
  EXPECT_TRUE(HasErrorContaining(report, "Nested Loop bodies are not supported"))
      << FormatCompileReport(report);
  EXPECT_EQ(FindInstruction(program->GetInstructions(LN_Event::OnFixedUpdate),
                            LN_OpCode::QuitGame),
            nullptr);
}

TEST(LN_TreeCompiler, BooleanEdgeOutputsShareOneFixedUpdateSample)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNodeSocketValueBoolean condition_default = {};
  condition_default.value = true;
  blender::bNode edge = {};
  blender::bNodeSocket condition = {};
  blender::bNodeSocket rising = {};
  blender::bNodeSocket falling = {};
  InitNode(edge, "LogicNativeBooleanEdge", "Boolean Edge", 1);
  InitSocket(condition,
             "Condition",
             blender::SOCK_BOOLEAN,
             &condition_default,
             "NodeSocketLogicCondition");
  InitSocket(rising, "Rising", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(falling, "Falling", blender::SOCK_BOOLEAN, nullptr);
  Append(edge.inputs, condition);
  Append(edge.outputs, rising);
  Append(edge.outputs, falling);
  Append(tree.nodes, edge);

  blender::bNode quit = {};
  blender::bNodeSocket quit_flow = {};
  InitNode(quit, "LogicNativeQuitGame", "Quit Game", 2);
  InitSocket(quit_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  Append(quit.inputs, quit_flow);
  Append(tree.nodes, quit);

  blender::bNode restart = {};
  blender::bNodeSocket restart_flow = {};
  InitNode(restart, "LogicNativeRestartGame", "Restart Game", 3);
  InitSocket(restart_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  Append(restart.inputs, restart_flow);
  Append(tree.nodes, restart);

  blender::bNodeLink rising_to_quit = {};
  blender::bNodeLink falling_to_restart = {};
  InitLink(rising_to_quit, edge, rising, quit, quit_flow);
  InitLink(falling_to_restart, edge, falling, restart, restart_flow);
  Append(tree.links, rising_to_quit);
  Append(tree.links, falling_to_restart);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  EXPECT_TRUE(program->GetInstructions(LN_Event::OnInit).empty());

  const LN_Instruction *quit_instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::QuitGame);
  const LN_Instruction *restart_instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::RestartGame);
  ASSERT_NE(quit_instruction, nullptr);
  ASSERT_NE(restart_instruction, nullptr);

  uint32_t rising_index = LN_INVALID_INDEX;
  uint32_t falling_index = LN_INVALID_INDEX;
  for (const LN_Instruction &instruction : program->GetInstructions(LN_Event::OnFixedUpdate)) {
    if (instruction.opcode != LN_OpCode::BranchRoute) {
      continue;
    }
    const LN_BoolExpression *condition = BoolExpressionAt(*program, instruction.bool_expr_index);
    ASSERT_NE(condition, nullptr);
    if (condition->kind == LN_BoolExpressionKind::BooleanEdge) {
      EXPECT_EQ(rising_index, LN_INVALID_INDEX);
      rising_index = instruction.bool_expr_index;
    }
    else if (condition->kind == LN_BoolExpressionKind::BooleanEdgeFalling) {
      EXPECT_EQ(falling_index, LN_INVALID_INDEX);
      falling_index = instruction.bool_expr_index;
    }
  }

  const LN_BoolExpression *rising_expression = BoolExpressionAt(*program, rising_index);
  const LN_BoolExpression *falling_expression = BoolExpressionAt(*program, falling_index);
  ASSERT_NE(rising_expression, nullptr);
  ASSERT_NE(falling_expression, nullptr);
  EXPECT_EQ(rising_expression->kind, LN_BoolExpressionKind::BooleanEdge);
  EXPECT_EQ(falling_expression->kind, LN_BoolExpressionKind::BooleanEdgeFalling);
  EXPECT_EQ(falling_expression->input0, rising_index);
}

TEST(LN_TreeCompiler, CooldownCompilesImmediateAndLatentRoutesWithIndependentReset)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_init = {};
  blender::bNodeSocket init_out = {};
  InitNode(on_init, "LogicNativeOnInit", "On Init", 1);
  InitSocket(init_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_init.outputs, init_out);
  Append(tree.nodes, on_init);

  blender::bNode on_update = {};
  blender::bNodeSocket update_out = {};
  InitNode(on_update, "LogicNativeOnFixedUpdate", "On Fixed Update", 2);
  InitSocket(update_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, update_out);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueFloat duration_default = {};
  duration_default.value = 0.25f;
  blender::bNodeSocketValueBoolean ignore_default = {};
  ignore_default.value = true;
  blender::bNode cooldown = {};
  blender::bNodeSocket cooldown_flow = {};
  blender::bNodeSocket cooldown_reset = {};
  blender::bNodeSocket cooldown_duration = {};
  blender::bNodeSocket cooldown_ignore = {};
  blender::bNodeSocket cooldown_accepted = {};
  blender::bNodeSocket cooldown_blocked = {};
  blender::bNodeSocket cooldown_completed = {};
  blender::bNodeSocket cooldown_remaining = {};
  blender::bNodeSocket cooldown_progress = {};
  blender::bNodeSocket cooldown_ready = {};
  InitNode(cooldown, "LogicNativeCooldown", "Cooldown", 3);
  InitSocket(cooldown_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(cooldown_reset, "Reset", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(cooldown_duration, "Duration", blender::SOCK_FLOAT, &duration_default);
  InitSocket(cooldown_ignore, "Ignore Timescale", blender::SOCK_BOOLEAN, &ignore_default);
  InitSocket(cooldown_accepted, "Accepted", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(cooldown_blocked, "Blocked", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(cooldown_completed, "Completed", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(cooldown_remaining, "Remaining", blender::SOCK_FLOAT, nullptr);
  InitSocket(cooldown_progress, "Progress", blender::SOCK_FLOAT, nullptr);
  InitSocket(cooldown_ready, "Is Ready", blender::SOCK_BOOLEAN, nullptr);
  Append(cooldown.inputs, cooldown_flow);
  Append(cooldown.inputs, cooldown_reset);
  Append(cooldown.inputs, cooldown_duration);
  Append(cooldown.inputs, cooldown_ignore);
  Append(cooldown.outputs, cooldown_accepted);
  Append(cooldown.outputs, cooldown_blocked);
  Append(cooldown.outputs, cooldown_completed);
  Append(cooldown.outputs, cooldown_remaining);
  Append(cooldown.outputs, cooldown_progress);
  Append(cooldown.outputs, cooldown_ready);
  Append(tree.nodes, cooldown);

  blender::bNode quit = {};
  blender::bNodeSocket quit_flow = {};
  InitNode(quit, "LogicNativeQuitGame", "Quit Game", 4);
  InitSocket(quit_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  Append(quit.inputs, quit_flow);
  Append(tree.nodes, quit);

  blender::bNode restart = {};
  blender::bNodeSocket restart_flow = {};
  InitNode(restart, "LogicNativeRestartGame", "Restart Game", 5);
  InitSocket(restart_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  Append(restart.inputs, restart_flow);
  Append(tree.nodes, restart);

  blender::bNode print_node = {};
  blender::bNodeSocket print_flow = {};
  blender::bNodeSocket print_value = {};
  InitNode(print_node, "LogicNativePrint", "Print", 6);
  InitSocket(print_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(print_value, "Value", blender::SOCK_CUSTOM, nullptr);
  Append(print_node.inputs, print_flow);
  Append(print_node.inputs, print_value);
  Append(tree.nodes, print_node);

  blender::bNodeLink init_to_flow = {};
  blender::bNodeLink update_to_reset = {};
  blender::bNodeLink accepted_to_quit = {};
  blender::bNodeLink blocked_to_restart = {};
  blender::bNodeLink completed_to_print = {};
  blender::bNodeLink remaining_to_print = {};
  InitLink(init_to_flow, on_init, init_out, cooldown, cooldown_flow);
  InitLink(update_to_reset, on_update, update_out, cooldown, cooldown_reset);
  InitLink(accepted_to_quit, cooldown, cooldown_accepted, quit, quit_flow);
  InitLink(blocked_to_restart, cooldown, cooldown_blocked, restart, restart_flow);
  InitLink(completed_to_print, cooldown, cooldown_completed, print_node, print_flow);
  InitLink(remaining_to_print, cooldown, cooldown_remaining, print_node, print_value);
  Append(tree.links, init_to_flow);
  Append(tree.links, update_to_reset);
  Append(tree.links, accepted_to_quit);
  Append(tree.links, blocked_to_restart);
  Append(tree.links, completed_to_print);
  Append(tree.links, remaining_to_print);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  ASSERT_EQ(program->GetTimeFlowStateCount(), 1u);

  const std::vector<LN_Instruction> &on_init_instructions = program->GetInstructions(
      LN_Event::OnInit);
  const LN_Instruction *attempt = FindInstruction(on_init_instructions, LN_OpCode::TryCooldown);
  ASSERT_NE(attempt, nullptr);
  EXPECT_NE(attempt->float_expr_index, LN_INVALID_INDEX);
  EXPECT_NE(attempt->secondary_bool_expr_index, LN_INVALID_INDEX);
  EXPECT_NE(FindInstruction(on_init_instructions, LN_OpCode::QuitGame), nullptr);
  EXPECT_NE(FindInstruction(on_init_instructions, LN_OpCode::RestartGame), nullptr);

  const std::vector<LN_Instruction> &fixed = program->GetInstructions(LN_Event::OnFixedUpdate);
  const LN_Instruction *reset = FindInstruction(fixed, LN_OpCode::ResetCooldown);
  ASSERT_NE(reset, nullptr);
  EXPECT_EQ(reset->int_value, attempt->int_value);
  const LN_Instruction *completed_print = FindInstruction(fixed, LN_OpCode::Print);
  ASSERT_NE(completed_print, nullptr);
  EXPECT_TRUE(BoolExpressionTreeContainsKind(
      *program, completed_print->bool_guard_expr_index, LN_BoolExpressionKind::CooldownCompleted));
  EXPECT_EQ(FindInstruction(fixed, LN_OpCode::QuitGame), nullptr);
  EXPECT_EQ(FindInstruction(fixed, LN_OpCode::RestartGame), nullptr);

  bool found_accepted_route = false;
  bool found_blocked_route = false;
  for (const LN_Instruction &instruction : on_init_instructions) {
    if (instruction.opcode != LN_OpCode::BranchRoute) {
      continue;
    }
    const LN_BoolExpression *condition = BoolExpressionAt(*program, instruction.bool_expr_index);
    ASSERT_NE(condition, nullptr);
    found_accepted_route |= condition->kind == LN_BoolExpressionKind::CooldownAccepted;
    found_blocked_route |= condition->kind == LN_BoolExpressionKind::CooldownBlocked;
  }
  EXPECT_TRUE(found_accepted_route);
  EXPECT_TRUE(found_blocked_route);

  bool found_completed_route = false;
  for (const LN_Instruction &instruction : fixed) {
    if (instruction.opcode != LN_OpCode::BranchRoute) {
      continue;
    }
    const LN_BoolExpression *condition = BoolExpressionAt(*program, instruction.bool_expr_index);
    ASSERT_NE(condition, nullptr);
    found_completed_route |= condition->kind == LN_BoolExpressionKind::CooldownCompleted;
  }
  EXPECT_FALSE(found_completed_route);

  bool found_ready = false;
  for (const LN_BoolExpression &expression : program->GetBoolExpressions()) {
    if (expression.kind == LN_BoolExpressionKind::CooldownReady) {
      found_ready = true;
      EXPECT_EQ(expression.int_value, attempt->int_value);
    }
  }
  bool found_remaining = false;
  bool found_progress = false;
  for (const LN_FloatExpression &expression : program->GetFloatExpressions()) {
    if (expression.kind == LN_FloatExpressionKind::CooldownRemaining) {
      found_remaining = true;
      EXPECT_EQ(expression.int_value, attempt->int_value);
    }
    if (expression.kind == LN_FloatExpressionKind::CooldownProgress) {
      found_progress = true;
      EXPECT_EQ(expression.int_value, attempt->int_value);
    }
  }
  EXPECT_TRUE(found_ready);
  EXPECT_TRUE(found_remaining);
  EXPECT_TRUE(found_progress);
}

TEST(LN_TreeCompiler, CooldownDataOutputWithoutFlowKeepsIndependentBranchesActive)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_init = {};
  blender::bNodeSocket init_out = {};
  InitNode(on_init, "LogicNativeOnInit", "On Init", 1);
  InitSocket(init_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_init.outputs, init_out);
  Append(tree.nodes, on_init);

  blender::bNode on_update = {};
  blender::bNodeSocket update_out = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 2);
  InitSocket(update_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, update_out);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueFloat duration_default = {};
  duration_default.value = 1.0f;
  blender::bNodeSocketValueBoolean ignore_default = {};
  blender::bNode cooldown = {};
  blender::bNodeSocket cooldown_flow = {};
  blender::bNodeSocket cooldown_reset = {};
  blender::bNodeSocket cooldown_duration = {};
  blender::bNodeSocket cooldown_ignore = {};
  blender::bNodeSocket cooldown_accepted = {};
  blender::bNodeSocket cooldown_blocked = {};
  blender::bNodeSocket cooldown_completed = {};
  blender::bNodeSocket cooldown_remaining = {};
  blender::bNodeSocket cooldown_progress = {};
  blender::bNodeSocket cooldown_ready = {};
  InitNode(cooldown, "LogicNativeCooldown", "Cooldown", 3);
  InitSocket(cooldown_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(cooldown_reset, "Reset", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(cooldown_duration, "Duration", blender::SOCK_FLOAT, &duration_default);
  InitSocket(cooldown_ignore, "Ignore Timescale", blender::SOCK_BOOLEAN, &ignore_default);
  InitSocket(cooldown_accepted, "Accepted", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(cooldown_blocked, "Blocked", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(cooldown_completed, "Completed", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(cooldown_remaining, "Remaining", blender::SOCK_FLOAT, nullptr);
  InitSocket(cooldown_progress, "Progress", blender::SOCK_FLOAT, nullptr);
  InitSocket(cooldown_ready, "Is Ready", blender::SOCK_BOOLEAN, nullptr);
  Append(cooldown.inputs, cooldown_flow);
  Append(cooldown.inputs, cooldown_reset);
  Append(cooldown.inputs, cooldown_duration);
  Append(cooldown.inputs, cooldown_ignore);
  Append(cooldown.outputs, cooldown_accepted);
  Append(cooldown.outputs, cooldown_blocked);
  Append(cooldown.outputs, cooldown_completed);
  Append(cooldown.outputs, cooldown_remaining);
  Append(cooldown.outputs, cooldown_progress);
  Append(cooldown.outputs, cooldown_ready);
  Append(tree.nodes, cooldown);

  blender::bNode print_node = {};
  blender::bNodeSocket print_flow = {};
  blender::bNodeSocket print_value = {};
  InitNode(print_node, "LogicNativePrint", "Print", 4);
  InitSocket(print_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocketWithIdentifier(print_value, "Message", "Value", blender::SOCK_CUSTOM, nullptr);
  Append(print_node.inputs, print_flow);
  Append(print_node.inputs, print_value);
  Append(tree.nodes, print_node);

  blender::bNodeSocketValueFloat timescale_default = {};
  timescale_default.value = 1.0f;
  blender::bNode set_timescale = {};
  blender::bNodeSocket timescale_flow = {};
  blender::bNodeSocket timescale = {};
  InitNode(set_timescale, "LogicNativeSetTimescale", "Set Timescale", 5);
  InitSocket(timescale_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(timescale, "Timescale", blender::SOCK_FLOAT, &timescale_default);
  Append(set_timescale.inputs, timescale_flow);
  Append(set_timescale.inputs, timescale);
  Append(tree.nodes, set_timescale);

  blender::bNodeLink update_to_print = {};
  blender::bNodeLink remaining_to_print = {};
  blender::bNodeLink init_to_timescale = {};
  InitLink(update_to_print, on_update, update_out, print_node, print_flow);
  InitLink(remaining_to_print, cooldown, cooldown_remaining, print_node, print_value);
  InitLink(init_to_timescale, on_init, init_out, set_timescale, timescale_flow);
  Append(tree.links, update_to_print);
  Append(tree.links, remaining_to_print);
  Append(tree.links, init_to_timescale);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  EXPECT_TRUE(program->GetCompileReport().GetDisabledReason().empty());
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  EXPECT_NE(FindInstruction(program->GetInstructions(LN_Event::OnInit), LN_OpCode::SetTimeScale),
            nullptr);
  EXPECT_NE(FindInstruction(program->GetInstructions(LN_Event::OnUpdate), LN_OpCode::Print),
            nullptr);
  EXPECT_EQ(FindInstruction(program->GetInstructions(LN_Event::OnInit), LN_OpCode::TryCooldown),
            nullptr);
  EXPECT_EQ(FindInstruction(program->GetInstructions(LN_Event::OnUpdate), LN_OpCode::TryCooldown),
            nullptr);
  EXPECT_EQ(
      FindInstruction(program->GetInstructions(LN_Event::OnUpdate), LN_OpCode::ResetCooldown),
      nullptr);
  EXPECT_EQ(program->GetTimeFlowStateCount(), 1u);

  bool found_remaining = false;
  for (const LN_FloatExpression &expression : program->GetFloatExpressions()) {
    found_remaining |= expression.kind == LN_FloatExpressionKind::CooldownRemaining;
  }
  EXPECT_TRUE(found_remaining);
}

TEST(LN_TreeCompiler, CooldownOrdersResetBeforeAttemptOnTheSameEvent)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket update_out = {};
  InitNode(on_update, "LogicNativeOnFixedUpdate", "On Fixed Update", 1);
  InitSocket(update_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, update_out);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueFloat duration = {};
  duration.value = 1.0f;
  blender::bNodeSocketValueBoolean ignore = {};
  blender::bNode cooldown = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket reset = {};
  blender::bNodeSocket duration_socket = {};
  blender::bNodeSocket ignore_socket = {};
  blender::bNodeSocket accepted = {};
  blender::bNodeSocket blocked = {};
  blender::bNodeSocket completed = {};
  InitNode(cooldown, "LogicNativeCooldown", "Cooldown", 2);
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(reset, "Reset", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(duration_socket, "Duration", blender::SOCK_FLOAT, &duration);
  InitSocket(ignore_socket, "Ignore Timescale", blender::SOCK_BOOLEAN, &ignore);
  InitSocket(accepted, "Accepted", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(blocked, "Blocked", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(completed, "Completed", blender::SOCK_BOOLEAN, nullptr);
  Append(cooldown.inputs, flow);
  Append(cooldown.inputs, reset);
  Append(cooldown.inputs, duration_socket);
  Append(cooldown.inputs, ignore_socket);
  Append(cooldown.outputs, accepted);
  Append(cooldown.outputs, blocked);
  Append(cooldown.outputs, completed);
  Append(tree.nodes, cooldown);

  blender::bNode quit = {};
  blender::bNodeSocket quit_flow = {};
  InitNode(quit, "LogicNativeQuitGame", "Quit Game", 3);
  InitSocket(quit_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  Append(quit.inputs, quit_flow);
  Append(tree.nodes, quit);

  blender::bNodeLink update_to_flow = {};
  blender::bNodeLink update_to_reset = {};
  blender::bNodeLink accepted_to_quit = {};
  InitLink(update_to_flow, on_update, update_out, cooldown, flow);
  InitLink(update_to_reset, on_update, update_out, cooldown, reset);
  InitLink(accepted_to_quit, cooldown, accepted, quit, quit_flow);
  Append(tree.links, update_to_flow);
  Append(tree.links, update_to_reset);
  Append(tree.links, accepted_to_quit);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  const std::vector<LN_Instruction> &fixed = program->GetInstructions(LN_Event::OnFixedUpdate);
  const int reset_index = FindInstructionIndex(fixed, LN_OpCode::ResetCooldown);
  const int attempt_index = FindInstructionIndex(fixed, LN_OpCode::TryCooldown);
  ASSERT_GE(reset_index, 0);
  EXPECT_GT(attempt_index, reset_index);
  EXPECT_EQ(fixed[size_t(reset_index)].int_value, fixed[size_t(attempt_index)].int_value);

  int route_count = 0;
  for (const LN_Instruction &instruction : fixed) {
    if (instruction.opcode != LN_OpCode::BranchRoute) {
      continue;
    }
    route_count++;
    const LN_BoolExpression *condition = BoolExpressionAt(*program, instruction.bool_expr_index);
    ASSERT_NE(condition, nullptr);
    EXPECT_EQ(condition->kind, LN_BoolExpressionKind::CooldownAccepted);
  }
  EXPECT_EQ(route_count, 1) << "Unlinked Blocked and Completed outputs must not emit routes";
}

TEST(LN_TreeCompiler, BarrierUsesPrintDoneFlowAndBooleanCondition)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_init = {};
  blender::bNodeSocket on_init_pulse = {};
  InitNode(on_init, "LogicNativeOnInit", "On Init", 1);
  InitSocket(on_init_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_init.outputs, on_init_pulse);
  Append(tree.nodes, on_init);

  blender::bNodeSocketValueString message_default = {};
  SetCString(message_default.value, "hit");
  blender::bNode print_node = {};
  blender::bNodeSocket print_flow = {};
  blender::bNodeSocket print_message = {};
  blender::bNodeSocket print_done = {};
  InitNode(print_node, "LogicNativePrint", "Print", 2);
  InitSocket(print_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(print_message, "Value", blender::SOCK_STRING, &message_default);
  SetCString(print_message.identifier, "Message");
  InitSocket(print_done, "Done", blender::SOCK_BOOLEAN, nullptr);
  Append(print_node.inputs, print_flow);
  Append(print_node.inputs, print_message);
  Append(print_node.outputs, print_done);
  Append(tree.nodes, print_node);

  blender::bNodeSocketValueBoolean barrier_condition_default = {};
  barrier_condition_default.value = true;
  blender::bNodeSocketValueFloat barrier_time_default = {};
  barrier_time_default.value = 0.0f;
  blender::bNodeSocketValueBoolean barrier_ignore_timescale_default = {};
  blender::bNode barrier = {};
  blender::bNodeSocket barrier_flow = {};
  blender::bNodeSocket barrier_condition = {};
  blender::bNodeSocket barrier_time = {};
  blender::bNodeSocket barrier_ignore_timescale = {};
  blender::bNodeSocket barrier_out = {};
  InitNode(barrier, "LogicNativeBarrier", "Barrier", 3);
  InitSocket(barrier_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(barrier_condition, "Condition", blender::SOCK_BOOLEAN, &barrier_condition_default);
  InitSocket(barrier_time, "Time", blender::SOCK_FLOAT, &barrier_time_default);
  InitSocket(barrier_ignore_timescale,
             "Ignore Timescale",
             blender::SOCK_BOOLEAN,
             &barrier_ignore_timescale_default);
  InitSocket(barrier_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(barrier.inputs, barrier_flow);
  Append(barrier.inputs, barrier_condition);
  Append(barrier.inputs, barrier_time);
  Append(barrier.inputs, barrier_ignore_timescale);
  Append(barrier.outputs, barrier_out);
  Append(tree.nodes, barrier);

  blender::bNode remove_object = {};
  blender::bNodeSocket remove_flow = {};
  blender::bNodeSocket remove_target = {};
  InitNode(remove_object, "LogicNativeRemoveObject", "Remove Object", 4);
  InitSocket(remove_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(remove_target, "Object", blender::SOCK_OBJECT, nullptr);
  Append(remove_object.inputs, remove_flow);
  Append(remove_object.inputs, remove_target);
  Append(tree.nodes, remove_object);

  blender::bNodeLink init_to_print = {};
  blender::bNodeLink print_to_barrier = {};
  blender::bNodeLink barrier_to_remove = {};
  InitLink(init_to_print, on_init, on_init_pulse, print_node, print_flow);
  InitLink(print_to_barrier, print_node, print_done, barrier, barrier_flow);
  InitLink(barrier_to_remove, barrier, barrier_out, remove_object, remove_flow);
  Append(tree.links, init_to_print);
  Append(tree.links, print_to_barrier);
  Append(tree.links, barrier_to_remove);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  EXPECT_EQ(program->GetTimeFlowStateCount(), 1u);

  EXPECT_NE(FindInstruction(program->GetInstructions(LN_Event::OnInit), LN_OpCode::Print),
            nullptr);
  const LN_Instruction *barrier_instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnInit), LN_OpCode::UpdateBarrier);
  ASSERT_NE(barrier_instruction, nullptr);
  EXPECT_NE(barrier_instruction->bool_expr_index, LN_INVALID_INDEX);
  EXPECT_NE(barrier_instruction->float_expr_index, LN_INVALID_INDEX);
  EXPECT_NE(barrier_instruction->bool_guard_expr_index, LN_INVALID_INDEX);

  const LN_Instruction *barrier_route = FindBranchRouteWithConditionKind(
      *program,
      program->GetInstructions(LN_Event::OnFixedUpdate),
      LN_BoolExpressionKind::BarrierPassed);
  ASSERT_NE(barrier_route, nullptr);
  EXPECT_EQ(barrier_route->bool_guard_expr_index, LN_INVALID_INDEX);

  const LN_Instruction *remove_instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::RemoveObject);
  ASSERT_NE(remove_instruction, nullptr);
  ExpectInstructionGuardedByRouteCondition(
      *program,
      LN_Event::OnFixedUpdate,
      *remove_instruction,
      LN_BoolExpressionKind::BarrierPassed);
}

TEST(LN_TreeCompiler, DelayAndPulsifyCompileAsTimeFlowContinuations)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_init = {};
  blender::bNodeSocket on_init_pulse = {};
  InitNode(on_init, "LogicNativeOnInit", "On Init", 1);
  InitSocket(on_init_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_init.outputs, on_init_pulse);
  Append(tree.nodes, on_init);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnFixedUpdate", "On Fixed Update", 2);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueFloat delay_default = {};
  delay_default.value = 0.25f;
  blender::bNodeSocketValueBoolean delay_ignore_timescale_default = {};
  blender::bNode delay = {};
  blender::bNodeSocket delay_flow = {};
  blender::bNodeSocket delay_duration = {};
  blender::bNodeSocket delay_ignore_timescale = {};
  blender::bNodeSocket delay_out = {};
  InitNode(delay, "LogicNativeDelay", "Delay", 3);
  InitSocket(delay_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(delay_duration, "Delay", blender::SOCK_FLOAT, &delay_default);
  InitSocket(delay_ignore_timescale,
             "Ignore Timescale",
             blender::SOCK_BOOLEAN,
             &delay_ignore_timescale_default);
  InitSocket(delay_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(delay.inputs, delay_flow);
  Append(delay.inputs, delay_duration);
  Append(delay.inputs, delay_ignore_timescale);
  Append(delay.outputs, delay_out);
  Append(tree.nodes, delay);

  blender::bNodeSocketValueFloat gap_default = {};
  gap_default.value = 0.5f;
  blender::bNodeSocketValueBoolean pulsify_ignore_timescale_default = {};
  blender::bNode pulsify = {};
  blender::bNodeSocket pulsify_flow = {};
  blender::bNodeSocket pulsify_gap = {};
  blender::bNodeSocket pulsify_ignore_timescale = {};
  blender::bNodeSocket pulsify_out = {};
  InitNode(pulsify, "LogicNativePulsify", "Pulsify", 4);
  InitSocket(pulsify_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(pulsify_gap, "Gap", blender::SOCK_FLOAT, &gap_default);
  InitSocket(pulsify_ignore_timescale,
             "Ignore Timescale",
             blender::SOCK_BOOLEAN,
             &pulsify_ignore_timescale_default);
  InitSocket(pulsify_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(pulsify.inputs, pulsify_flow);
  Append(pulsify.inputs, pulsify_gap);
  Append(pulsify.inputs, pulsify_ignore_timescale);
  Append(pulsify.outputs, pulsify_out);
  Append(tree.nodes, pulsify);

  blender::bNode quit = {};
  blender::bNodeSocket quit_flow = {};
  InitNode(quit, "LogicNativeQuitGame", "Quit Game", 5);
  InitSocket(quit_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  Append(quit.inputs, quit_flow);
  Append(tree.nodes, quit);

  blender::bNode restart = {};
  blender::bNodeSocket restart_flow = {};
  InitNode(restart, "LogicNativeRestartGame", "Restart Game", 6);
  InitSocket(restart_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  Append(restart.inputs, restart_flow);
  Append(tree.nodes, restart);

  blender::bNodeLink init_to_delay = {};
  blender::bNodeLink delay_to_quit = {};
  blender::bNodeLink update_to_pulsify = {};
  blender::bNodeLink pulsify_to_restart = {};
  InitLink(init_to_delay, on_init, on_init_pulse, delay, delay_flow);
  InitLink(delay_to_quit, delay, delay_out, quit, quit_flow);
  InitLink(update_to_pulsify, on_update, on_update_pulse, pulsify, pulsify_flow);
  InitLink(pulsify_to_restart, pulsify, pulsify_out, restart, restart_flow);
  Append(tree.links, init_to_delay);
  Append(tree.links, delay_to_quit);
  Append(tree.links, update_to_pulsify);
  Append(tree.links, pulsify_to_restart);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  EXPECT_EQ(program->GetTimeFlowStateCount(), 2u);

  EXPECT_NE(FindInstruction(program->GetInstructions(LN_Event::OnInit), LN_OpCode::ArmDelay),
            nullptr);
  EXPECT_NE(
      FindInstruction(program->GetInstructions(LN_Event::OnFixedUpdate),
                      LN_OpCode::UpdatePulsify),
      nullptr);

  const LN_Instruction *quit_instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::QuitGame);
  ASSERT_NE(quit_instruction, nullptr);
  const LN_Instruction *delay_route = FindBranchRouteWithConditionKind(
      *program,
      program->GetInstructions(LN_Event::OnFixedUpdate),
      LN_BoolExpressionKind::DelayDone);
  ASSERT_NE(delay_route, nullptr);
  EXPECT_EQ(delay_route->bool_guard_expr_index, LN_INVALID_INDEX);
  EXPECT_TRUE(BoolExpressionTreeContainsKind(*program,
                                             quit_instruction->bool_guard_expr_index,
                                             LN_BoolExpressionKind::InstructionExecuted));

  const LN_Instruction *restart_instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::RestartGame);
  ASSERT_NE(restart_instruction, nullptr);
  const LN_Instruction *pulsify_route = FindBranchRouteWithConditionKind(
      *program,
      program->GetInstructions(LN_Event::OnFixedUpdate),
      LN_BoolExpressionKind::PulsifyPulse);
  ASSERT_NE(pulsify_route, nullptr);
  EXPECT_EQ(pulsify_route->bool_guard_expr_index, LN_INVALID_INDEX);
  EXPECT_TRUE(BoolExpressionTreeContainsKind(*program,
                                             restart_instruction->bool_guard_expr_index,
                                             LN_BoolExpressionKind::InstructionExecuted));
}

TEST(LN_TreeCompiler, DelayInheritsFrameUpdateDomainFromItsTrigger)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket update_out = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Frame Update", 1);
  InitSocket(update_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, update_out);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueFloat duration_default = {};
  duration_default.value = 0.25f;
  blender::bNodeSocketValueBoolean ignore_timescale_default = {};
  blender::bNode delay = {};
  blender::bNodeSocket delay_flow = {};
  blender::bNodeSocket duration = {};
  blender::bNodeSocket ignore_timescale = {};
  blender::bNodeSocket delay_out = {};
  InitNode(delay, "LogicNativeDelay", "Delay", 2);
  InitSocket(delay_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(duration, "Delay", blender::SOCK_FLOAT, &duration_default);
  InitSocket(ignore_timescale,
             "Ignore Timescale",
             blender::SOCK_BOOLEAN,
             &ignore_timescale_default);
  InitSocket(delay_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(delay.inputs, delay_flow);
  Append(delay.inputs, duration);
  Append(delay.inputs, ignore_timescale);
  Append(delay.outputs, delay_out);
  Append(tree.nodes, delay);

  blender::bNodeSocketValueString message_default = {};
  SetCString(message_default.value, "frame delay");
  blender::bNode print_node = {};
  blender::bNodeSocket print_flow = {};
  blender::bNodeSocket message = {};
  InitNode(print_node, "LogicNativePrint", "Print", 3);
  InitSocket(print_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(message, "Message", blender::SOCK_STRING, &message_default);
  Append(print_node.inputs, print_flow);
  Append(print_node.inputs, message);
  Append(tree.nodes, print_node);

  blender::bNodeLink update_to_delay = {};
  blender::bNodeLink delay_to_print = {};
  InitLink(update_to_delay, on_update, update_out, delay, delay_flow);
  InitLink(delay_to_print, delay, delay_out, print_node, print_flow);
  Append(tree.links, update_to_delay);
  Append(tree.links, delay_to_print);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  EXPECT_NE(FindInstruction(program->GetInstructions(LN_Event::OnUpdate), LN_OpCode::ArmDelay),
            nullptr);
  EXPECT_EQ(FindInstruction(program->GetInstructions(LN_Event::OnFixedUpdate),
                            LN_OpCode::ArmDelay),
            nullptr);
  const LN_Instruction *print = FindInstruction(program->GetInstructions(LN_Event::OnUpdate),
                                                LN_OpCode::Print);
  ASSERT_NE(print, nullptr);
  ExpectInstructionGuardedByRouteCondition(
      *program, LN_Event::OnUpdate, *print, LN_BoolExpressionKind::DelayDone);
}

TEST(LN_TreeCompiler, LatentTimeNodeRejectsMixedFrameAndFixedClockDomains)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket update_out = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Frame Update", 1);
  InitSocket(update_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, update_out);
  Append(tree.nodes, on_update);

  blender::bNode on_fixed = {};
  blender::bNodeSocket fixed_out = {};
  InitNode(on_fixed, "LogicNativeOnFixedUpdate", "On Logic + Physics Step", 2);
  InitSocket(fixed_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_fixed.outputs, fixed_out);
  Append(tree.nodes, on_fixed);

  blender::bNode any_flow = {};
  blender::bNodeSocket any_in = {};
  blender::bNodeSocket any_out = {};
  InitNode(any_flow, "LogicNativeAnyFlow", "Any Flow", 3);
  InitSocket(any_in, "In", blender::SOCK_BOOLEAN, nullptr);
  any_in.flag |= blender::SOCK_MULTI_INPUT;
  InitSocket(any_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(any_flow.inputs, any_in);
  Append(any_flow.outputs, any_out);
  Append(tree.nodes, any_flow);

  blender::bNodeSocketValueFloat duration_default = {};
  duration_default.value = 0.25f;
  blender::bNodeSocketValueBoolean ignore_timescale_default = {};
  blender::bNode delay = {};
  blender::bNodeSocket delay_flow = {};
  blender::bNodeSocket duration = {};
  blender::bNodeSocket ignore_timescale = {};
  blender::bNodeSocket delay_out = {};
  InitNode(delay, "LogicNativeDelay", "Delay", 4);
  InitSocket(delay_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(duration, "Delay", blender::SOCK_FLOAT, &duration_default);
  InitSocket(ignore_timescale,
             "Ignore Timescale",
             blender::SOCK_BOOLEAN,
             &ignore_timescale_default);
  InitSocket(delay_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(delay.inputs, delay_flow);
  Append(delay.inputs, duration);
  Append(delay.inputs, ignore_timescale);
  Append(delay.outputs, delay_out);
  Append(tree.nodes, delay);

  blender::bNodeLink update_to_any = {};
  blender::bNodeLink fixed_to_any = {};
  blender::bNodeLink any_to_delay = {};
  InitLink(update_to_any, on_update, update_out, any_flow, any_in);
  InitLink(fixed_to_any, on_fixed, fixed_out, any_flow, any_in);
  InitLink(any_to_delay, any_flow, any_out, delay, delay_flow);
  update_to_any.multi_input_sort_id = 1;
  fixed_to_any.multi_input_sort_id = 0;
  Append(tree.links, update_to_any);
  Append(tree.links, fixed_to_any);
  Append(tree.links, any_to_delay);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_TRUE(program->GetCompileReport().HasErrors());
  EXPECT_TRUE(HasErrorContaining(program->GetCompileReport(),
                                 "cannot mix Frame Update and Logic + Physics trigger paths"))
      << FormatCompileReport(program->GetCompileReport());
}

TEST(LN_TreeCompiler, WaitForNextFrameArmsInSourceDomainAndAlwaysResumesOnFrameUpdate)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_fixed = {};
  blender::bNodeSocket fixed_out = {};
  InitNode(on_fixed, "LogicNativeOnFixedUpdate", "On Logic + Physics Step", 1);
  InitSocket(fixed_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_fixed.outputs, fixed_out);
  Append(tree.nodes, on_fixed);

  blender::bNode next_frame = {};
  blender::bNodeSocket next_flow = {};
  blender::bNodeSocket next_out = {};
  InitNode(next_frame, "LogicNativeOnNextFrame", "Wait for Next Frame", 2);
  InitSocket(next_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(next_out, "Completed", blender::SOCK_BOOLEAN, nullptr);
  Append(next_frame.inputs, next_flow);
  Append(next_frame.outputs, next_out);
  Append(tree.nodes, next_frame);

  blender::bNodeSocketValueString message_default = {};
  SetCString(message_default.value, "next frame");
  blender::bNode print_node = {};
  blender::bNodeSocket print_flow = {};
  blender::bNodeSocket message = {};
  InitNode(print_node, "LogicNativePrint", "Print", 3);
  InitSocket(print_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(message, "Message", blender::SOCK_STRING, &message_default);
  Append(print_node.inputs, print_flow);
  Append(print_node.inputs, message);
  Append(tree.nodes, print_node);

  blender::bNodeLink fixed_to_next = {};
  blender::bNodeLink next_to_print = {};
  InitLink(fixed_to_next, on_fixed, fixed_out, next_frame, next_flow);
  InitLink(next_to_print, next_frame, next_out, print_node, print_flow);
  Append(tree.links, fixed_to_next);
  Append(tree.links, next_to_print);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  EXPECT_EQ(program->GetTimeFlowStateCount(), 1u);

  EXPECT_NE(FindInstruction(program->GetInstructions(LN_Event::OnFixedUpdate),
                            LN_OpCode::ArmNextFrame),
            nullptr);
  EXPECT_EQ(FindInstruction(program->GetInstructions(LN_Event::OnUpdate),
                            LN_OpCode::ArmNextFrame),
            nullptr);

  const std::vector<LN_Instruction> &update_instructions = program->GetInstructions(
      LN_Event::OnUpdate);
  const LN_Instruction *route = FindBranchRouteWithConditionKind(
      *program, update_instructions, LN_BoolExpressionKind::NextFrameDone);
  ASSERT_NE(route, nullptr);
  const LN_Instruction *print = FindInstruction(update_instructions, LN_OpCode::Print);
  ASSERT_NE(print, nullptr);
  ExpectInstructionGuardedByRouteCondition(
      *program, LN_Event::OnUpdate, *print, LN_BoolExpressionKind::NextFrameDone);
}

TEST(LN_TreeCompiler, LatentCompletionDropsExpiredConstraintBreakGuard)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNodeSocketValueString constraint_name_default = {};
  SetCString(constraint_name_default.value, "breakable_joint");
  blender::bNodeSocketValueBoolean match_any_default = {};
  match_any_default.value = true;
  blender::bNode constraint_break = {};
  blender::bNodeSocket constraint_object = {};
  blender::bNodeSocket constraint_name = {};
  blender::bNodeSocket match_any = {};
  blender::bNodeSocket broken = {};
  InitNode(constraint_break, "LogicNativeOnConstraintBreak", "On Constraint Break", 1);
  InitSocket(constraint_object, "Object", blender::SOCK_OBJECT, nullptr, "NodeSocketLogicObject");
  InitSocket(constraint_name,
             "Constraint Name",
             blender::SOCK_STRING,
             &constraint_name_default,
             "NodeSocketLogicString");
  InitSocket(
      match_any, "Match Any", blender::SOCK_BOOLEAN, &match_any_default, "NodeSocketLogicBool");
  InitSocket(broken, "Broken", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  Append(constraint_break.inputs, constraint_object);
  Append(constraint_break.inputs, constraint_name);
  Append(constraint_break.inputs, match_any);
  Append(constraint_break.outputs, broken);
  Append(tree.nodes, constraint_break);

  blender::bNodeSocketValueFloat delay_default = {};
  delay_default.value = 1.0f;
  blender::bNodeSocketValueBoolean ignore_timescale_default = {};
  blender::bNode delay = {};
  blender::bNodeSocket delay_flow = {};
  blender::bNodeSocket delay_duration = {};
  blender::bNodeSocket delay_ignore_timescale = {};
  blender::bNodeSocket delay_out = {};
  InitNode(delay, "LogicNativeDelay", "Delay", 2);
  InitSocket(delay_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(delay_duration, "Delay", blender::SOCK_FLOAT, &delay_default);
  InitSocket(delay_ignore_timescale,
             "Ignore Timescale",
             blender::SOCK_BOOLEAN,
             &ignore_timescale_default);
  InitSocket(delay_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(delay.inputs, delay_flow);
  Append(delay.inputs, delay_duration);
  Append(delay.inputs, delay_ignore_timescale);
  Append(delay.outputs, delay_out);
  Append(tree.nodes, delay);

  blender::bNode remove_object = {};
  blender::bNodeSocket remove_flow = {};
  blender::bNodeSocket remove_target = {};
  InitNode(remove_object, "LogicNativeRemoveObject", "Remove Object", 3);
  InitSocket(remove_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(remove_target, "Object", blender::SOCK_OBJECT, nullptr);
  Append(remove_object.inputs, remove_flow);
  Append(remove_object.inputs, remove_target);
  Append(tree.nodes, remove_object);

  blender::bNodeLink break_to_delay = {};
  blender::bNodeLink delay_to_remove = {};
  InitLink(break_to_delay, constraint_break, broken, delay, delay_flow);
  InitLink(delay_to_remove, delay, delay_out, remove_object, remove_flow);
  Append(tree.links, break_to_delay);
  Append(tree.links, delay_to_remove);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  const LN_Instruction *arm_delay = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::ArmDelay);
  ASSERT_NE(arm_delay, nullptr);
  EXPECT_NE(arm_delay->bool_guard_expr_index, LN_INVALID_INDEX);

  const LN_Instruction *completion_route = FindBranchRouteWithConditionKind(
      *program,
      program->GetInstructions(LN_Event::OnFixedUpdate),
      LN_BoolExpressionKind::DelayDone);
  ASSERT_NE(completion_route, nullptr);
  EXPECT_EQ(completion_route->bool_guard_expr_index, LN_INVALID_INDEX);

  const LN_Instruction *remove = FindInstruction(program->GetInstructions(LN_Event::OnFixedUpdate),
                                                 LN_OpCode::RemoveObject);
  ASSERT_NE(remove, nullptr);
  EXPECT_NE(remove->bool_guard_expr_index, LN_INVALID_INDEX);
}

TEST(LN_TreeCompiler, TimerCompilesOnInitArmAndFixedUpdateContinuation)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_init = {};
  blender::bNodeSocket on_init_pulse = {};
  InitNode(on_init, "LogicNativeOnInit", "On Init", 1);
  InitSocket(on_init_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_init.outputs, on_init_pulse);
  Append(tree.nodes, on_init);

  blender::bNodeSocketValueFloat seconds_default = {};
  seconds_default.value = 0.25f;
  blender::bNodeSocketValueBoolean timer_ignore_timescale_default = {};
  blender::bNode timer = {};
  blender::bNodeSocket timer_flow = {};
  blender::bNodeSocket timer_seconds = {};
  blender::bNodeSocket timer_ignore_timescale = {};
  blender::bNodeSocket timer_out = {};
  InitNode(timer, "LogicNativeTimer", "Timer", 2);
  InitSocket(timer_flow, "Set Timer", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(timer_seconds, "Seconds", blender::SOCK_FLOAT, &seconds_default);
  InitSocket(timer_ignore_timescale,
             "Ignore Timescale",
             blender::SOCK_BOOLEAN,
             &timer_ignore_timescale_default);
  InitSocket(timer_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(timer.inputs, timer_flow);
  Append(timer.inputs, timer_seconds);
  Append(timer.inputs, timer_ignore_timescale);
  Append(timer.outputs, timer_out);
  Append(tree.nodes, timer);

  blender::bNode quit = {};
  blender::bNodeSocket quit_flow = {};
  InitNode(quit, "LogicNativeQuitGame", "Quit Game", 3);
  InitSocket(quit_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  Append(quit.inputs, quit_flow);
  Append(tree.nodes, quit);

  blender::bNodeLink init_to_timer = {};
  blender::bNodeLink timer_to_quit = {};
  InitLink(init_to_timer, on_init, on_init_pulse, timer, timer_flow);
  InitLink(timer_to_quit, timer, timer_out, quit, quit_flow);
  Append(tree.links, init_to_timer);
  Append(tree.links, timer_to_quit);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  EXPECT_EQ(program->GetTimerStateCount(), 1u);

  const std::vector<LN_Instruction> &on_init_instructions = program->GetInstructions(
      LN_Event::OnInit);
  const LN_Instruction *arm_timer = FindInstruction(on_init_instructions, LN_OpCode::ArmTimer);
  ASSERT_NE(arm_timer, nullptr);
  EXPECT_EQ(arm_timer->int_value, 0);
  EXPECT_NE(arm_timer->float_expr_index, LN_INVALID_INDEX);

  const std::vector<LN_Instruction> &fixed_instructions = program->GetInstructions(
      LN_Event::OnFixedUpdate);
  ASSERT_EQ(fixed_instructions.size(), 2u);
  const LN_Instruction *route = FindBranchRouteWithConditionKind(
      *program, fixed_instructions, LN_BoolExpressionKind::TimerElapsed);
  ASSERT_NE(route, nullptr);
  EXPECT_EQ(route->bool_guard_expr_index, LN_INVALID_INDEX);
  const LN_BoolExpression *route_condition = BoolExpressionAt(*program, route->bool_expr_index);
  ASSERT_NE(route_condition, nullptr);
  EXPECT_EQ(route_condition->kind, LN_BoolExpressionKind::TimerElapsed);
  EXPECT_EQ(route_condition->int_value, 0);

  const LN_Instruction *quit_instruction = FindInstruction(fixed_instructions,
                                                           LN_OpCode::QuitGame);
  ASSERT_NE(quit_instruction, nullptr);
  const LN_BoolExpression *guard = BoolExpressionAt(*program,
                                                    quit_instruction->bool_guard_expr_index);
  ASSERT_NE(guard, nullptr);
  EXPECT_EQ(guard->kind, LN_BoolExpressionKind::InstructionExecuted);
  ASSERT_LT(guard->input0, fixed_instructions.size());
  EXPECT_EQ(fixed_instructions[guard->input0].opcode, LN_OpCode::BranchRoute);
}

TEST(LN_TreeCompiler, GenericVectorInputBuildsValueExpressionAfterTypedVectorCache)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_init = {};
  blender::bNodeSocket on_init_pulse = {};
  InitNode(on_init, "LogicNativeOnInit", "On Init", 1);
  InitSocket(on_init_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_init.outputs, on_init_pulse);
  Append(tree.nodes, on_init);

  blender::bNodeSocketValueFloat x_default = {};
  blender::bNodeSocketValueFloat y_default = {};
  blender::bNodeSocketValueFloat z_default = {};
  blender::bNodeSocketValueFloat w_default = {};
  x_default.value = 1.5f;
  y_default.value = 2.5f;
  z_default.value = 3.5f;
  w_default.value = 4.5f;
  blender::bNode combine = {};
  blender::bNodeSocket combine_x = {};
  blender::bNodeSocket combine_y = {};
  blender::bNodeSocket combine_z = {};
  blender::bNodeSocket combine_w = {};
  blender::bNodeSocket combine_vector = {};
  InitNode(combine, "LogicNativeCombineXYZW", "Combine XYZW", 2);
  InitSocket(combine_x, "X", blender::SOCK_FLOAT, &x_default);
  InitSocket(combine_y, "Y", blender::SOCK_FLOAT, &y_default);
  InitSocket(combine_z, "Z", blender::SOCK_FLOAT, &z_default);
  InitSocket(combine_w, "W", blender::SOCK_FLOAT, &w_default);
  InitSocket(combine_vector, "Vector", blender::SOCK_VECTOR, nullptr);
  Append(combine.inputs, combine_x);
  Append(combine.inputs, combine_y);
  Append(combine.inputs, combine_z);
  Append(combine.inputs, combine_w);
  Append(combine.outputs, combine_vector);
  Append(tree.nodes, combine);

  blender::bNode typecast = {};
  blender::bNodeSocket typecast_value = {};
  blender::bNodeSocket typecast_string = {};
  InitNode(typecast, "LogicNativeTypecast", "Typecast", 3);
  typecast.custom1 = 2;
  InitSocket(typecast_value, "Value", blender::SOCK_CUSTOM, nullptr);
  InitSocket(typecast_string, "String", blender::SOCK_STRING, nullptr);
  Append(typecast.inputs, typecast_value);
  Append(typecast.outputs, typecast_string);
  Append(tree.nodes, typecast);

  blender::bNodeSocketValueString property_name_default = {};
  SetCString(property_name_default.value, "vector4_text");
  blender::bNode set_property = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket property_object = {};
  blender::bNodeSocket property_name = {};
  blender::bNodeSocket property_value = {};
  InitNode(set_property, "LogicNativeSetGamePropertyString", "Set Text", 4);
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(property_object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(property_name, "Property", blender::SOCK_STRING, &property_name_default);
  InitSocket(property_value, "Value", blender::SOCK_STRING, nullptr);
  Append(set_property.inputs, flow);
  Append(set_property.inputs, property_object);
  Append(set_property.inputs, property_name);
  Append(set_property.inputs, property_value);
  Append(tree.nodes, set_property);

  blender::bNodeLink flow_link = {};
  blender::bNodeLink combine_to_typecast = {};
  blender::bNodeLink typecast_to_property = {};
  InitLink(flow_link, on_init, on_init_pulse, set_property, flow);
  InitLink(combine_to_typecast, combine, combine_vector, typecast, typecast_value);
  InitLink(typecast_to_property, typecast, typecast_string, set_property, property_value);
  Append(tree.links, flow_link);
  Append(tree.links, combine_to_typecast);
  Append(tree.links, typecast_to_property);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors());

  const LN_Instruction *instruction = FindInstruction(program->GetInstructions(LN_Event::OnInit),
                                                      LN_OpCode::SetGameProperty);
  ASSERT_NE(instruction, nullptr);
  const LN_StringExpression *string_expr = StringExpressionAt(*program,
                                                              instruction->string_expr_index);
  ASSERT_NE(string_expr, nullptr);
  EXPECT_EQ(string_expr->kind, LN_StringExpressionKind::FromGenericValue);
  const LN_ValueExpression *value_expr = ValueExpressionAt(*program,
                                                           string_expr->value_expr_index);
  ASSERT_NE(value_expr, nullptr);
  EXPECT_EQ(value_expr->kind, LN_ValueExpressionKind::CombineVector4);
  EXPECT_EQ(value_expr->value.type, LN_ValueType::Vector4);
  EXPECT_NEAR(value_expr->value.vector4_value.w(), 4.5f, 0.0001f);
}

TEST(LN_TreeCompiler, CompareWithALiveInputDrivesABranch)
{
  /* "If the property is greater than 1": Compare used to compile only with constant inputs. */
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Frame Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueString read_name_default = {};
  SetCString(read_name_default.value, "count");
  blender::bNode read = {};
  blender::bNodeSocket read_object = {};
  blender::bNodeSocket read_name = {};
  blender::bNodeSocket read_value = {};
  InitNode(read, "LogicNativeGetGamePropertyFloat", "Get Property", 2);
  InitSocket(read_object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(read_name, "Property", blender::SOCK_STRING, &read_name_default);
  InitSocketWithIdentifier(read_value, "Property Value", "Value", blender::SOCK_FLOAT, nullptr);
  Append(read.inputs, read_object);
  Append(read.inputs, read_name);
  Append(read.outputs, read_value);
  Append(tree.nodes, read);

  blender::bNodeSocketValueFloat b_default = {};
  b_default.value = 1.0f;
  blender::bNode compare = {};
  blender::bNodeSocket compare_a = {};
  blender::bNodeSocket compare_b = {};
  blender::bNodeSocket compare_result = {};
  InitNode(compare, "LogicNativeCompare", "Compare", 3);
  compare.custom1 = 2; /* Greater Than */
  InitSocket(compare_a, "A", blender::SOCK_FLOAT, nullptr);
  InitSocket(compare_b, "B", blender::SOCK_FLOAT, &b_default);
  InitSocket(compare_result, "Result", blender::SOCK_BOOLEAN, nullptr);
  Append(compare.inputs, compare_a);
  Append(compare.inputs, compare_b);
  Append(compare.outputs, compare_result);
  Append(tree.nodes, compare);

  blender::bNode branch = {};
  blender::bNodeSocket branch_flow = {};
  blender::bNodeSocket branch_condition = {};
  blender::bNodeSocket branch_true = {};
  InitNode(branch, "LogicNativeBranch", "Branch", 4);
  InitSocket(branch_flow, "Flow", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  InitSocket(branch_condition, "Condition", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(branch_true, "True", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  Append(branch.inputs, branch_flow);
  Append(branch.inputs, branch_condition);
  Append(branch.outputs, branch_true);
  Append(tree.nodes, branch);

  blender::bNodeSocketValueString write_name_default = {};
  SetCString(write_name_default.value, "many");
  blender::bNodeSocketValueBoolean write_value_default = {};
  write_value_default.value = true;
  blender::bNode write = {};
  blender::bNodeSocket write_flow = {};
  blender::bNodeSocket write_object = {};
  blender::bNodeSocket write_name = {};
  blender::bNodeSocket write_value = {};
  InitNode(write, "LogicNativeSetGamePropertyBool", "Set Property", 5);
  InitSocket(write_flow, "Flow", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  InitSocket(write_object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(write_name, "Property", blender::SOCK_STRING, &write_name_default);
  InitSocket(write_value, "Value", blender::SOCK_BOOLEAN, &write_value_default);
  Append(write.inputs, write_flow);
  Append(write.inputs, write_object);
  Append(write.inputs, write_name);
  Append(write.inputs, write_value);
  Append(tree.nodes, write);

  std::array<blender::bNodeLink, 4> links = {};
  InitLink(links[0], on_update, on_update_pulse, branch, branch_flow);
  InitLink(links[1], read, read_value, compare, compare_a);
  InitLink(links[2], compare, compare_result, branch, branch_condition);
  InitLink(links[3], branch, branch_true, write, write_flow);
  for (blender::bNodeLink &link : links) {
    Append(tree.links, link);
  }

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(program, nullptr);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  EXPECT_NE(FindInstruction(program->GetInstructions(LN_Event::OnUpdate), LN_OpCode::SetGameProperty),
            nullptr);
  const std::vector<LN_BoolExpression> &expressions = program->GetBoolExpressions();
  EXPECT_TRUE(std::any_of(expressions.begin(), expressions.end(), [](const LN_BoolExpression &e) {
    return e.kind == LN_BoolExpressionKind::FloatCompare &&
           e.float_compare_operation == LN_FloatCompareOperation::GreaterThan;
  }));
}

TEST(LN_TreeCompiler, AnyValueOutputLinkedToATextInputKeepsItsSource)
{
  /* Get Object Property's Value (any value) feeds a text input directly, without a Typecast. */
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_init = {};
  blender::bNodeSocket on_init_pulse = {};
  InitNode(on_init, "LogicNativeOnInit", "On Init", 1);
  InitSocket(on_init_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_init.outputs, on_init_pulse);
  Append(tree.nodes, on_init);

  blender::bNodeSocketValueString source_name_default = {};
  SetCString(source_name_default.value, "item");
  blender::bNode source = {};
  blender::bNodeSocket source_object = {};
  blender::bNodeSocket source_name = {};
  blender::bNodeSocket source_value = {};
  InitNode(source, "LogicNativeGetGameProperty", "Get Object Property", 2);
  InitSocket(source_object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(source_name, "Property", blender::SOCK_STRING, &source_name_default);
  InitSocket(source_value, "Value", blender::SOCK_CUSTOM, nullptr, "NodeSocketLogicGeneric");
  Append(source.inputs, source_object);
  Append(source.inputs, source_name);
  Append(source.outputs, source_value);
  Append(tree.nodes, source);

  blender::bNodeSocketValueString property_name_default = {};
  SetCString(property_name_default.value, "item_name");
  blender::bNode set_property = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket property_object = {};
  blender::bNodeSocket property_name = {};
  blender::bNodeSocket property_value = {};
  InitNode(set_property, "LogicNativeSetGamePropertyString", "Set Text", 3);
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(property_object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(property_name, "Property", blender::SOCK_STRING, &property_name_default);
  InitSocket(property_value, "Value", blender::SOCK_STRING, nullptr);
  Append(set_property.inputs, flow);
  Append(set_property.inputs, property_object);
  Append(set_property.inputs, property_name);
  Append(set_property.inputs, property_value);
  Append(tree.nodes, set_property);

  std::array<blender::bNodeLink, 2> links = {};
  InitLink(links[0], on_init, on_init_pulse, set_property, flow);
  InitLink(links[1], source, source_value, set_property, property_value);
  for (blender::bNodeLink &link : links) {
    Append(tree.links, link);
  }

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(program, nullptr);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  const LN_Instruction *instruction = FindInstruction(program->GetInstructions(LN_Event::OnInit),
                                                      LN_OpCode::SetGameProperty);
  ASSERT_NE(instruction, nullptr);
  const LN_StringExpression *string_expr = StringExpressionAt(*program,
                                                              instruction->string_expr_index);
  ASSERT_NE(string_expr, nullptr);
  EXPECT_EQ(string_expr->kind, LN_StringExpressionKind::FromGenericValue);
  /* The runtime reads the source from value_expr_index; anything else turns the text into
   * "None". */
  const LN_ValueExpression *value_expr = ValueExpressionAt(*program,
                                                           string_expr->value_expr_index);
  ASSERT_NE(value_expr, nullptr);
  EXPECT_EQ(value_expr->kind, LN_ValueExpressionKind::ObjectGameProperty);
}

TEST(LN_TreeCompiler, TypecastIntegerToFloatUsesDirectTypedExpressions)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnFixedUpdate", "On Logic + Physics Step", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNode mouse_wheel = {};
  blender::bNodeSocket when_scrolled = {};
  blender::bNodeSocket difference = {};
  InitNode(mouse_wheel, "LogicNativeMouseWheel", "Mouse Wheel", 2);
  InitSocket(when_scrolled, "When Scrolled", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(difference, "Difference", blender::SOCK_INT, nullptr);
  Append(mouse_wheel.outputs, when_scrolled);
  Append(mouse_wheel.outputs, difference);
  Append(tree.nodes, mouse_wheel);

  blender::bNode typecast = {};
  blender::bNodeSocket typecast_value = {};
  blender::bNodeSocket typecast_float = {};
  InitNode(typecast, "LogicNativeTypecast", "Typecast", 3);
  typecast.custom1 = 3;
  InitSocket(typecast_value, "Value", blender::SOCK_CUSTOM, nullptr);
  InitSocket(typecast_float, "Float", blender::SOCK_FLOAT, nullptr);
  Append(typecast.inputs, typecast_value);
  Append(typecast.outputs, typecast_float);
  Append(tree.nodes, typecast);

  blender::bNodeSocketValueString property_name_default = {};
  SetCString(property_name_default.value, "wheel_delta");
  blender::bNode set_property = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket object = {};
  blender::bNodeSocket property = {};
  blender::bNodeSocket value = {};
  InitNode(set_property, "LogicNativeSetGamePropertyFloat", "Set Wheel Delta", 4);
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(property, "Property", blender::SOCK_STRING, &property_name_default);
  InitSocket(value, "Value", blender::SOCK_FLOAT, nullptr);
  Append(set_property.inputs, flow);
  Append(set_property.inputs, object);
  Append(set_property.inputs, property);
  Append(set_property.inputs, value);
  Append(tree.nodes, set_property);

  blender::bNodeLink flow_link = {};
  blender::bNodeLink source_link = {};
  blender::bNodeLink value_link = {};
  InitLink(flow_link, on_update, on_update_pulse, set_property, flow);
  InitLink(source_link, mouse_wheel, difference, typecast, typecast_value);
  InitLink(value_link, typecast, typecast_float, set_property, value);
  Append(tree.links, flow_link);
  Append(tree.links, source_link);
  Append(tree.links, value_link);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());

  const LN_Instruction *instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::SetGameProperty);
  ASSERT_NE(instruction, nullptr);
  const LN_FloatExpression *conversion = FloatExpressionAt(*program,
                                                           instruction->float_expr_index);
  ASSERT_NE(conversion, nullptr);
  EXPECT_EQ(conversion->kind, LN_FloatExpressionKind::FromInt);
  const LN_IntExpression *source = IntExpressionAt(*program, conversion->input0);
  ASSERT_NE(source, nullptr);
  EXPECT_EQ(source->kind, LN_IntExpressionKind::MouseWheelDelta);

  for (const LN_ValueExpression &expression : program->GetValueExpressions()) {
    EXPECT_NE(expression.kind, LN_ValueExpressionKind::FromInt);
  }

  bool has_direct_conversion = false;
  for (const LN_RegisterExpressionOp &op : program->GetRegisterExpressionProgram().ops) {
    has_direct_conversion |= op.kind == LN_RegisterExpressionOpKind::FloatFromInt;
  }
  EXPECT_TRUE(has_direct_conversion);
  EXPECT_TRUE(program->ValidateRegisterExpressionIR());
}

TEST(LN_TreeCompiler, TypecastFloatToVectorBroadcastsWithoutGenericValueBoxing)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnFixedUpdate", "On Logic + Physics Step", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNode get_timescale = {};
  blender::bNodeSocket timescale = {};
  InitNode(get_timescale, "LogicNativeGetTimescale", "Get Timescale", 2);
  InitSocket(timescale, "Timescale", blender::SOCK_FLOAT, nullptr);
  Append(get_timescale.outputs, timescale);
  Append(tree.nodes, get_timescale);

  blender::bNode typecast = {};
  blender::bNodeSocket typecast_value = {};
  blender::bNodeSocket typecast_vector = {};
  InitNode(typecast, "LogicNativeTypecast", "Typecast", 3);
  typecast.custom1 = 4;
  InitSocket(typecast_value, "Value", blender::SOCK_CUSTOM, nullptr);
  InitSocket(typecast_vector, "Vector", blender::SOCK_VECTOR, nullptr);
  Append(typecast.inputs, typecast_value);
  Append(typecast.outputs, typecast_vector);
  Append(tree.nodes, typecast);

  blender::bNode set_gravity = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket gravity = {};
  InitNode(set_gravity, "LogicNativeSetGravity", "Set Gravity", 4);
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(gravity, "Gravity", blender::SOCK_VECTOR, nullptr);
  Append(set_gravity.inputs, flow);
  Append(set_gravity.inputs, gravity);
  Append(tree.nodes, set_gravity);

  blender::bNodeLink flow_link = {};
  blender::bNodeLink source_link = {};
  blender::bNodeLink value_link = {};
  InitLink(flow_link, on_update, on_update_pulse, set_gravity, flow);
  InitLink(source_link, get_timescale, timescale, typecast, typecast_value);
  InitLink(value_link, typecast, typecast_vector, set_gravity, gravity);
  Append(tree.links, flow_link);
  Append(tree.links, source_link);
  Append(tree.links, value_link);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());

  const LN_Instruction *instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::SetGravity);
  ASSERT_NE(instruction, nullptr);
  const LN_VectorExpression *conversion = VectorExpressionAt(*program,
                                                             instruction->vector_expr_index);
  ASSERT_NE(conversion, nullptr);
  EXPECT_EQ(conversion->kind, LN_VectorExpressionKind::BroadcastFloat);
  const LN_FloatExpression *source = FloatExpressionAt(*program, conversion->float_expr_index);
  ASSERT_NE(source, nullptr);
  EXPECT_EQ(source->kind, LN_FloatExpressionKind::SnapshotTimeScale);

  for (const LN_ValueExpression &expression : program->GetValueExpressions()) {
    EXPECT_NE(expression.kind, LN_ValueExpressionKind::FromFloat);
  }

  bool has_direct_conversion = false;
  for (const LN_RegisterExpressionOp &op : program->GetRegisterExpressionProgram().ops) {
    has_direct_conversion |= op.kind == LN_RegisterExpressionOpKind::VectorBroadcastFloat;
  }
  EXPECT_TRUE(has_direct_conversion);
  EXPECT_TRUE(program->ValidateRegisterExpressionIR());
}

TEST(LN_TreeCompiler, RejectsFrameUpdateFlowForSimulationMutation)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket update_out = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Frame Update", 1);
  InitSocket(update_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, update_out);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueVector gravity_default = {};
  gravity_default.value[2] = -9.81f;
  blender::bNode set_gravity = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket gravity = {};
  InitNode(set_gravity, "LogicNativeSetGravity", "Set Gravity", 2);
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(gravity, "Gravity", blender::SOCK_VECTOR, &gravity_default);
  Append(set_gravity.inputs, flow);
  Append(set_gravity.inputs, gravity);
  Append(tree.nodes, set_gravity);

  blender::bNodeLink link = {};
  InitLink(link, on_update, update_out, set_gravity, flow);
  Append(tree.links, link);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_TRUE(program->GetCompileReport().HasErrors());
  EXPECT_TRUE(HasErrorContaining(program->GetCompileReport(), "requires Logic + Physics flow"))
      << FormatCompileReport(program->GetCompileReport());
  EXPECT_EQ(FindInstruction(program->GetInstructions(LN_Event::OnUpdate), LN_OpCode::SetGravity),
            nullptr);
}

TEST(LN_TreeCompiler, UIRoutesMayPlaceObjectsButNotPushThem)
{
  /* A UI event may place an object (a tooltip, a sliding panel); setting a velocity stays a
   * Logic + Physics job. */
  for (const int attribute : {0 /* World Position */, 2 /* World Linear Velocity */}) {
    blender::bNodeTree tree = {};
    InitTree(tree);
    blender::bNodeSocketValueString action_default = {};
    SetCString(action_default.value, "button");
    blender::bNode on_ui_event = {};
    blender::bNodeSocket action_input = {};
    blender::bNodeSocket out = {};
    InitNode(on_ui_event, "LogicNativeOnUIEvent", "On UI Event", 1);
    on_ui_event.custom1 = int16_t(LN_UIElementEvent::Click);
    InitSocket(action_input, "Action", blender::SOCK_STRING, &action_default);
    InitSocket(out, "Out", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
    Append(on_ui_event.inputs, action_input);
    Append(on_ui_event.outputs, out);
    Append(tree.nodes, on_ui_event);

    blender::bNodeSocketValueVector value_default = {};
    value_default.value[0] = 2.0f;
    blender::bNode set_attribute = {};
    SetObjectAttributeAuxSockets aux;
    blender::bNodeSocket flow = {};
    blender::bNodeSocket value = {};
    InitSetObjectAttributeVectorNode(
        set_attribute, aux, flow, value, "Set Object Attribute", 2, attribute, &value_default);
    Append(tree.nodes, set_attribute);

    blender::bNodeLink link = {};
    InitLink(link, on_ui_event, out, set_attribute, flow);
    Append(tree.links, link);

    const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
    ASSERT_NE(program, nullptr);
    if (attribute == 0) {
      EXPECT_FALSE(program->GetCompileReport().HasErrors())
          << FormatCompileReport(program->GetCompileReport());
      EXPECT_FALSE(program->GetInstructions(LN_Event::OnUpdate).empty());
    }
    else {
      EXPECT_TRUE(HasErrorContaining(program->GetCompileReport(), "requires Logic + Physics flow"))
          << FormatCompileReport(program->GetCompileReport());
    }
  }
}

TEST(LN_TreeCompiler, MouseOverDrivesVisualRotationOnLogicPhysicsByDefault)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode mouse_over = {};
  blender::bNodeSocket mouse_object = {};
  blender::bNodeSocket mouse_camera = {};
  blender::bNodeSocket on_over = {};
  InitNode(mouse_over, "LogicNativeMouseOver", "Mouse Over", 1);
  InitSocket(mouse_object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(mouse_camera, "Camera", blender::SOCK_OBJECT, nullptr);
  InitSocket(on_over, "On Over", blender::SOCK_BOOLEAN, nullptr);
  Append(mouse_over.inputs, mouse_object);
  Append(mouse_over.inputs, mouse_camera);
  Append(mouse_over.outputs, on_over);
  Append(tree.nodes, mouse_over);

  blender::bNodeSocketValueRotation rotation_default = {};
  rotation_default.value_euler[2] = 0.5f;
  blender::bNodeSocketValueBoolean local_default = {};
  blender::bNode apply_rotation = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket object = {};
  blender::bNodeSocket rotation = {};
  blender::bNodeSocket local = {};
  InitNode(apply_rotation, "LogicNativeApplyRotation", "Apply Rotation", 2);
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(rotation, "Rotation", blender::SOCK_ROTATION, &rotation_default);
  InitSocket(local, "Local", blender::SOCK_BOOLEAN, &local_default);
  Append(apply_rotation.inputs, flow);
  Append(apply_rotation.inputs, object);
  Append(apply_rotation.inputs, rotation);
  Append(apply_rotation.inputs, local);
  Append(tree.nodes, apply_rotation);

  blender::bNodeLink flow_link = {};
  InitLink(flow_link, mouse_over, on_over, apply_rotation, flow);
  Append(tree.links, flow_link);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(program, nullptr);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  const LN_Instruction *instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::ApplyTransformVector);
  ASSERT_NE(instruction, nullptr);
  EXPECT_EQ(LN_VectorOperationChannel(instruction->vector_operation_channel),
            LN_VectorOperationChannel::Rotation);
  EXPECT_EQ(FindInstruction(program->GetInstructions(LN_Event::OnUpdate),
                            LN_OpCode::ApplyTransformVector),
            nullptr);
}

TEST(LN_TreeCompiler, MouseOverObjectOutputUsesTheSharedMouseOverQuery)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_fixed = {};
  blender::bNodeSocket fixed_out = {};
  InitNode(on_fixed, "LogicNativeOnFixedUpdate", "On Fixed Update", 1);
  InitSocket(fixed_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_fixed.outputs, fixed_out);
  Append(tree.nodes, on_fixed);

  blender::bNode mouse_over = {};
  blender::bNodeSocket mouse_object = {};
  blender::bNodeSocket mouse_camera = {};
  blender::bNodeSocket hit_object = {};
  blender::bNodeSocket hit_point = {};
  InitNode(mouse_over, "LogicNativeMouseOver", "Mouse Over", 2);
  InitSocket(mouse_object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(mouse_camera, "Camera", blender::SOCK_OBJECT, nullptr);
  InitSocket(hit_object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(hit_point, "Point", blender::SOCK_VECTOR, nullptr);
  Append(mouse_over.inputs, mouse_object);
  Append(mouse_over.inputs, mouse_camera);
  Append(mouse_over.outputs, hit_object);
  Append(mouse_over.outputs, hit_point);
  Append(tree.nodes, mouse_over);

  blender::bNode print_node = {};
  blender::bNodeSocket print_flow = {};
  blender::bNodeSocket print_message = {};
  InitNode(print_node, "LogicNativePrint", "Print", 3);
  InitSocket(print_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(print_message, "Message", blender::SOCK_CUSTOM, nullptr);
  Append(print_node.inputs, print_flow);
  Append(print_node.inputs, print_message);
  Append(tree.nodes, print_node);

  blender::bNode print_point_node = {};
  blender::bNodeSocket print_point_flow = {};
  blender::bNodeSocket print_point_message = {};
  InitNode(print_point_node, "LogicNativePrint", "Print Point", 4);
  InitSocket(print_point_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(print_point_message, "Message", blender::SOCK_CUSTOM, nullptr);
  Append(print_point_node.inputs, print_point_flow);
  Append(print_point_node.inputs, print_point_message);
  Append(tree.nodes, print_point_node);

  blender::bNodeLink flow_link = {};
  blender::bNodeLink point_flow_link = {};
  blender::bNodeLink object_link = {};
  blender::bNodeLink point_link = {};
  InitLink(flow_link, on_fixed, fixed_out, print_node, print_flow);
  InitLink(point_flow_link, on_fixed, fixed_out, print_point_node, print_point_flow);
  InitLink(object_link, mouse_over, hit_object, print_node, print_message);
  InitLink(point_link, mouse_over, hit_point, print_point_node, print_point_message);
  Append(tree.links, flow_link);
  Append(tree.links, point_flow_link);
  Append(tree.links, object_link);
  Append(tree.links, point_link);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(program, nullptr);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());

  const LN_ValueExpression *object_expression = nullptr;
  int print_count = 0;
  for (const LN_Instruction &instruction : program->GetInstructions(LN_Event::OnFixedUpdate)) {
    if (instruction.opcode != LN_OpCode::Print) {
      continue;
    }
    print_count++;
    const LN_ValueExpression *expression = ValueExpressionAt(*program,
                                                              instruction.value_expr_index);
    if (expression != nullptr &&
        expression->kind == LN_ValueExpressionKind::PhysicsQueryObject)
    {
      object_expression = expression;
    }
  }
  EXPECT_EQ(print_count, 2);
  ASSERT_NE(object_expression, nullptr);
  ASSERT_LT(object_expression->input0, program->GetQueryExpressions().size());
  EXPECT_EQ(program->GetQueryExpressions()[object_expression->input0].kind,
            LN_QueryExpressionKind::MouseOver);
  ASSERT_EQ(program->GetQueryExpressions().size(), 2u);
  EXPECT_EQ(program->GetQueryExpressions()[0].runtime_state_index,
            program->GetQueryExpressions()[1].runtime_state_index);
  EXPECT_EQ(program->GetQueryRuntimeStateCount(), 1u);
}

TEST(LN_TreeCompiler, WhilePausedCanConsumeMouseOverConditionOnFrameUpdate)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode while_paused = {};
  blender::bNodeSocket pause_object = {};
  blender::bNodeSocket pause_collection = {};
  blender::bNodeSocket pause_update = {};
  InitNode(while_paused, "LogicNativeWhilePaused", "While Paused", 1);
  while_paused.custom1 = int(LN_PauseProcessingScope::Owner);
  InitSocket(pause_object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(pause_collection, "Collection", blender::SOCK_COLLECTION, nullptr);
  InitSocket(pause_update, "Update", blender::SOCK_BOOLEAN, nullptr);
  Append(while_paused.inputs, pause_object);
  Append(while_paused.inputs, pause_collection);
  Append(while_paused.outputs, pause_update);
  Append(tree.nodes, while_paused);

  blender::bNode mouse_over = {};
  blender::bNodeSocket mouse_object = {};
  blender::bNodeSocket mouse_camera = {};
  blender::bNodeSocket is_over = {};
  InitNode(mouse_over, "LogicNativeMouseOver", "Mouse Over", 2);
  InitSocket(mouse_object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(mouse_camera, "Camera", blender::SOCK_OBJECT, nullptr);
  InitSocket(is_over, "Is Over", blender::SOCK_BOOLEAN, nullptr);
  Append(mouse_over.inputs, mouse_object);
  Append(mouse_over.inputs, mouse_camera);
  Append(mouse_over.outputs, is_over);
  Append(tree.nodes, mouse_over);

  blender::bNodeSocketValueBoolean condition_default = {};
  blender::bNode branch = {};
  blender::bNodeSocket branch_flow = {};
  blender::bNodeSocket branch_condition = {};
  blender::bNodeSocket branch_true = {};
  InitNode(branch, "LogicNativeBranch", "Branch", 3);
  InitSocket(branch_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(branch_condition, "Condition", blender::SOCK_BOOLEAN, &condition_default);
  InitSocket(branch_true, "True", blender::SOCK_BOOLEAN, nullptr);
  Append(branch.inputs, branch_flow);
  Append(branch.inputs, branch_condition);
  Append(branch.outputs, branch_true);
  Append(tree.nodes, branch);

  blender::bNodeSocketValueRotation rotation_default = {};
  rotation_default.value_euler[2] = 0.5f;
  blender::bNodeSocketValueBoolean local_default = {};
  blender::bNode apply_rotation = {};
  blender::bNodeSocket rotation_flow = {};
  blender::bNodeSocket rotation_object = {};
  blender::bNodeSocket rotation = {};
  blender::bNodeSocket local = {};
  InitNode(apply_rotation, "LogicNativeApplyRotation", "Apply Rotation", 4);
  InitSocket(rotation_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(rotation_object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(rotation, "Rotation", blender::SOCK_ROTATION, &rotation_default);
  InitSocket(local, "Local", blender::SOCK_BOOLEAN, &local_default);
  Append(apply_rotation.inputs, rotation_flow);
  Append(apply_rotation.inputs, rotation_object);
  Append(apply_rotation.inputs, rotation);
  Append(apply_rotation.inputs, local);
  Append(tree.nodes, apply_rotation);

  blender::bNodeLink pause_to_branch = {};
  blender::bNodeLink condition_to_branch = {};
  blender::bNodeLink branch_to_rotation = {};
  InitLink(pause_to_branch, while_paused, pause_update, branch, branch_flow);
  InitLink(condition_to_branch, mouse_over, is_over, branch, branch_condition);
  InitLink(branch_to_rotation, branch, branch_true, apply_rotation, rotation_flow);
  Append(tree.links, pause_to_branch);
  Append(tree.links, condition_to_branch);
  Append(tree.links, branch_to_rotation);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(program, nullptr);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  const LN_Instruction *instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnUpdate), LN_OpCode::ApplyTransformVector);
  ASSERT_NE(instruction, nullptr);
  EXPECT_EQ(LN_VectorOperationChannel(instruction->vector_operation_channel),
            LN_VectorOperationChannel::Rotation);
  EXPECT_EQ(FindInstruction(program->GetInstructions(LN_Event::OnFixedUpdate),
                            LN_OpCode::ApplyTransformVector),
            nullptr);
}

TEST(LN_TreeCompiler, TweenRotationThroughSeparateEulerFeedsSetOrientation)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnFixedUpdate", "On Fixed Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueRotation from_default = {};
  blender::bNodeSocketValueRotation to_default = {};
  to_default.value_euler[2] = 3.14159265f;
  blender::bNodeSocketValueFloat duration_default = {};
  duration_default.value = 1.0f;
  blender::bNodeSocketValueBoolean ignore_timescale_default = {};
  blender::bNode tween = {};
  blender::bNodeSocket tween_forward = {};
  blender::bNodeSocket tween_back = {};
  blender::bNodeSocket tween_from_float = {};
  blender::bNodeSocket tween_to_float = {};
  blender::bNodeSocket tween_from_vector = {};
  blender::bNodeSocket tween_to_vector = {};
  blender::bNodeSocket tween_from = {};
  blender::bNodeSocket tween_to = {};
  blender::bNodeSocket tween_duration = {};
  blender::bNodeSocket tween_ignore_timescale = {};
  blender::bNodeSocket tween_done = {};
  blender::bNodeSocket tween_reached = {};
  blender::bNodeSocket tween_result_float = {};
  blender::bNodeSocket tween_result_vector = {};
  blender::bNodeSocket tween_result = {};
  blender::bNodeSocket tween_factor = {};
  InitNode(tween, "LogicNativeTweenValue", "Tween Value", 2);
  tween.custom1 = 2;
  tween.custom2 = 1;
  InitSocket(tween_forward, "Forward", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(tween_back, "Back", blender::SOCK_BOOLEAN, nullptr);
  InitSocketWithIdentifier(tween_from_float, "FromFloat", "From", blender::SOCK_FLOAT, nullptr);
  InitSocketWithIdentifier(tween_to_float, "ToFloat", "To", blender::SOCK_FLOAT, nullptr);
  InitSocketWithIdentifier(tween_from_vector, "FromVector", "From", blender::SOCK_VECTOR, nullptr);
  InitSocketWithIdentifier(tween_to_vector, "ToVector", "To", blender::SOCK_VECTOR, nullptr);
  InitSocketWithIdentifier(
      tween_from, "FromRotation", "From", blender::SOCK_ROTATION, &from_default);
  InitSocketWithIdentifier(tween_to, "ToRotation", "To", blender::SOCK_ROTATION, &to_default);
  InitSocket(tween_duration, "Duration", blender::SOCK_FLOAT, &duration_default);
  InitSocket(tween_ignore_timescale,
             "Ignore Timescale",
             blender::SOCK_BOOLEAN,
             &ignore_timescale_default);
  InitSocket(tween_done, "Done", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(tween_reached, "Reached", blender::SOCK_BOOLEAN, nullptr);
  InitSocketWithIdentifier(
      tween_result_float, "ResultFloat", "Result", blender::SOCK_FLOAT, nullptr);
  InitSocketWithIdentifier(
      tween_result_vector, "ResultVector", "Result", blender::SOCK_VECTOR, nullptr);
  InitSocketWithIdentifier(
      tween_result, "ResultRotation", "Result", blender::SOCK_ROTATION, nullptr);
  InitSocket(tween_factor, "Factor", blender::SOCK_FLOAT, nullptr);
  Append(tween.inputs, tween_forward);
  Append(tween.inputs, tween_back);
  Append(tween.inputs, tween_from_float);
  Append(tween.inputs, tween_to_float);
  Append(tween.inputs, tween_from_vector);
  Append(tween.inputs, tween_to_vector);
  Append(tween.inputs, tween_from);
  Append(tween.inputs, tween_to);
  Append(tween.inputs, tween_duration);
  Append(tween.inputs, tween_ignore_timescale);
  Append(tween.outputs, tween_done);
  Append(tween.outputs, tween_reached);
  Append(tween.outputs, tween_result_float);
  Append(tween.outputs, tween_result_vector);
  Append(tween.outputs, tween_result);
  Append(tween.outputs, tween_factor);
  Append(tree.nodes, tween);

  blender::bNode separate = {};
  blender::bNodeSocket separate_rotation = {};
  blender::bNodeSocket separate_x = {};
  blender::bNodeSocket separate_y = {};
  blender::bNodeSocket separate_z = {};
  InitNode(separate, "LogicNativeSeparateEuler", "Separate Rotation", 3);
  InitSocket(separate_rotation, "Rotation", blender::SOCK_ROTATION, nullptr);
  InitSocket(separate_x, "X", blender::SOCK_FLOAT, nullptr);
  InitSocket(separate_y, "Y", blender::SOCK_FLOAT, nullptr);
  InitSocket(separate_z, "Z", blender::SOCK_FLOAT, nullptr);
  Append(separate.inputs, separate_rotation);
  Append(separate.outputs, separate_x);
  Append(separate.outputs, separate_y);
  Append(separate.outputs, separate_z);
  Append(tree.nodes, separate);

  blender::bNode euler = {};
  blender::bNodeSocket euler_x = {};
  blender::bNodeSocket euler_y = {};
  blender::bNodeSocket euler_z = {};
  blender::bNodeSocket euler_rotation = {};
  InitNode(euler, "LogicNativeEuler", "Combine Rotation", 4);
  InitSocket(euler_x, "X", blender::SOCK_FLOAT, nullptr);
  InitSocket(euler_y, "Y", blender::SOCK_FLOAT, nullptr);
  InitSocket(euler_z, "Z", blender::SOCK_FLOAT, nullptr);
  InitSocketWithIdentifier(
      euler_rotation, "Rotation", "Rotation", blender::SOCK_ROTATION, nullptr);
  Append(euler.inputs, euler_x);
  Append(euler.inputs, euler_y);
  Append(euler.inputs, euler_z);
  Append(euler.outputs, euler_rotation);
  Append(tree.nodes, euler);

  blender::bNode set_orientation = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket xyz = {};
  blender::bNodeSocket object = {};
  blender::bNodeSocket value = {};
  blender::bNodeSocket position = {};
  blender::bNodeSocket rotation = {};
  blender::bNodeSocket scale = {};
  blender::bNodeSocket color = {};
  blender::bNodeSocket visible = {};
  blender::bNodeSocket include_children = {};
  blender::bNodeSocket teleport = {};
  blender::bNodeSocket done = {};
  InitNode(set_orientation, "LogicNativeSetObjectAttribute", "Set Object Attribute", 5);
  set_orientation.custom1 = 1;
  set_orientation.custom2 = 15;
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(xyz, "XYZ", blender::SOCK_VECTOR, nullptr);
  InitSocket(object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(value, "Value", blender::SOCK_VECTOR, nullptr);
  InitSocket(position, "Position", blender::SOCK_VECTOR, nullptr);
  InitSocket(rotation, "Rotation", blender::SOCK_ROTATION, nullptr);
  InitSocket(scale, "Scale", blender::SOCK_VECTOR, nullptr);
  InitSocket(color, "Color", blender::SOCK_RGBA, nullptr);
  InitSocket(visible, "Visible", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(include_children, "Include Children", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(teleport, "Teleport", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(done, "Done", blender::SOCK_BOOLEAN, nullptr);
  Append(set_orientation.inputs, flow);
  Append(set_orientation.inputs, xyz);
  Append(set_orientation.inputs, object);
  Append(set_orientation.inputs, value);
  Append(set_orientation.inputs, position);
  Append(set_orientation.inputs, rotation);
  Append(set_orientation.inputs, scale);
  Append(set_orientation.inputs, color);
  Append(set_orientation.inputs, visible);
  Append(set_orientation.inputs, include_children);
  Append(set_orientation.inputs, teleport);
  Append(set_orientation.outputs, done);
  Append(tree.nodes, set_orientation);

  blender::bNodeLink flow_link = {};
  blender::bNodeLink tween_to_separate = {};
  blender::bNodeLink separate_x_to_euler = {};
  blender::bNodeLink separate_y_to_euler = {};
  blender::bNodeLink separate_z_to_euler = {};
  blender::bNodeLink euler_to_set = {};
  InitLink(flow_link, on_update, on_update_pulse, set_orientation, flow);
  InitLink(tween_to_separate, tween, tween_result, separate, separate_rotation);
  InitLink(separate_x_to_euler, separate, separate_x, euler, euler_x);
  InitLink(separate_y_to_euler, separate, separate_y, euler, euler_y);
  InitLink(separate_z_to_euler, separate, separate_z, euler, euler_z);
  InitLink(euler_to_set, euler, euler_rotation, set_orientation, rotation);
  Append(tree.links, flow_link);
  Append(tree.links, tween_to_separate);
  Append(tree.links, separate_x_to_euler);
  Append(tree.links, separate_y_to_euler);
  Append(tree.links, separate_z_to_euler);
  Append(tree.links, euler_to_set);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());

  const LN_Instruction *instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::SetTransformVector);
  ASSERT_NE(instruction, nullptr);
  EXPECT_EQ(instruction->vector_operation_channel,
            uint8_t(LN_VectorOperationChannel::Orientation));

  const LN_VectorExpression *combined = InstructionVectorExpression(*program, *instruction);
  ASSERT_NE(combined, nullptr);
  EXPECT_EQ(combined->kind, LN_VectorExpressionKind::Combine);
  EXPECT_EQ(combined->output_type, LN_ValueType::Rotation);

  const LN_FloatExpression *z_component = FloatExpressionAt(*program, combined->input2);
  ASSERT_NE(z_component, nullptr);
  EXPECT_EQ(z_component->kind, LN_FloatExpressionKind::VectorComponent);
  EXPECT_EQ(z_component->component_index, 2);

  const LN_VectorExpression *tween_rotation = VectorExpressionAt(*program, z_component->input0);
  ASSERT_NE(tween_rotation, nullptr);
  EXPECT_EQ(tween_rotation->kind, LN_VectorExpressionKind::TweenRotationResult);
  EXPECT_EQ(tween_rotation->output_type, LN_ValueType::Rotation);
}

TEST(LN_TreeCompiler, EmitsCommandsChainedFromStopActionDoneOutput)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnFixedUpdate", "On Fixed Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueInt layer_default = {};
  layer_default.value = 0;
  blender::bNode stop_action = {};
  blender::bNodeSocket stop_flow = {};
  blender::bNodeSocket stop_layer = {};
  blender::bNodeSocket stop_done = {};
  InitNode(stop_action, "LogicNativeStopAction", "Stop Action", 2);
  InitSocket(stop_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(stop_layer, "Layer", blender::SOCK_INT, &layer_default);
  InitSocket(stop_done, "Done", blender::SOCK_BOOLEAN, nullptr);
  Append(stop_action.inputs, stop_flow);
  Append(stop_action.inputs, stop_layer);
  Append(stop_action.outputs, stop_done);
  Append(tree.nodes, stop_action);

  blender::bNodeSocketValueVector position_default = {};
  position_default.value[0] = 0.25f;
  position_default.value[1] = 0.5f;
  position_default.value[2] = 0.75f;
  blender::bNode set_position = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket position = {};
  SetObjectAttributeAuxSockets set_attribute_aux = {};
  InitSetObjectAttributeVectorNode(
      set_position, set_attribute_aux, flow, position, "Set Position", 3, 0, &position_default);
  Append(tree.nodes, set_position);

  blender::bNodeLink event_to_stop = {};
  blender::bNodeLink stop_done_to_position = {};
  InitLink(event_to_stop, on_update, on_update_pulse, stop_action, stop_flow);
  InitLink(stop_done_to_position, stop_action, stop_done, set_position, flow);
  Append(tree.links, event_to_stop);
  Append(tree.links, stop_done_to_position);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors());
  EXPECT_NE(nullptr,
            FindInstruction(program->GetInstructions(LN_Event::OnFixedUpdate),
                            LN_OpCode::StopAction));
  EXPECT_TRUE(HasVectorInstruction(program->GetInstructions(LN_Event::OnFixedUpdate),
                                   LN_OpCode::SetTransformVector,
                                   MT_Vector3(0.25f, 0.5f, 0.75f)));
}

TEST(LN_TreeCompiler, PlayActionCompilesExactAsynchronousLifecycleRoutes)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::ID action_id = {};
  *reinterpret_cast<uint16_t *>(action_id.name) = uint16_t(blender::ID_AC);
  std::strncpy(action_id.name + 2, "ButtonExit", sizeof(action_id.name) - 3);
  action_id.name[sizeof(action_id.name) - 1] = '\0';

  blender::bNode on_init = {};
  blender::bNodeSocket init_out = {};
  InitNode(on_init, "LogicNativeOnInit", "On Init", 1);
  InitSocket(init_out, "Out", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  Append(on_init.outputs, init_out);
  Append(tree.nodes, on_init);

  blender::bNodeSocketValueObject object_default = {};
  blender::bNodeSocketValueFloat start_default = {};
  blender::bNodeSocketValueFloat end_default = {};
  blender::bNodeSocketValueInt layer_default = {};
  blender::bNodeSocketValueInt priority_default = {};
  blender::bNodeSocketValueFloat blendin_default = {};
  blender::bNodeSocketValueFloat speed_default = {};
  end_default.value = 9.0f;
  speed_default.value = 1.0f;

  blender::bNode play = {};
  std::array<blender::bNodeSocket, 8> inputs = {};
  std::array<blender::bNodeSocket, 4> outputs = {};
  InitNode(play, "LogicNativePlayAction", "Play Action", 2);
  play.id = &action_id;
  InitSocket(inputs[0], "Flow", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  InitSocket(inputs[1], "Object", blender::SOCK_OBJECT, &object_default);
  InitSocket(inputs[2], "Start Frame", blender::SOCK_FLOAT, &start_default);
  InitSocket(inputs[3], "End Frame", blender::SOCK_FLOAT, &end_default);
  InitSocket(inputs[4], "Layer", blender::SOCK_INT, &layer_default);
  InitSocket(inputs[5], "Priority", blender::SOCK_INT, &priority_default);
  InitSocket(inputs[6], "Blend In", blender::SOCK_FLOAT, &blendin_default);
  InitSocket(inputs[7], "Speed", blender::SOCK_FLOAT, &speed_default);
  const std::array<const char *, 4> output_names = {
      "Started", "Completed", "Interrupted", "Failed"};
  for (blender::bNodeSocket &socket : inputs) {
    Append(play.inputs, socket);
  }
  for (size_t index = 0; index < outputs.size(); index++) {
    InitSocket(outputs[index],
               output_names[index],
               blender::SOCK_BOOLEAN,
               nullptr,
               "NodeSocketLogicExecution");
    Append(play.outputs, outputs[index]);
  }
  Append(tree.nodes, play);

  std::array<blender::bNode, 4> quits = {};
  std::array<blender::bNodeSocket, 4> quit_flows = {};
  std::array<blender::bNodeLink, 5> links = {};
  InitLink(links[0], on_init, init_out, play, inputs[0]);
  Append(tree.links, links[0]);
  for (size_t index = 0; index < quits.size(); index++) {
    InitNode(quits[index], "LogicNativeQuitGame", output_names[index], int32_t(3 + index));
    InitSocket(quit_flows[index],
               "Flow",
               blender::SOCK_BOOLEAN,
               nullptr,
               "NodeSocketLogicExecution");
    Append(quits[index].inputs, quit_flows[index]);
    Append(tree.nodes, quits[index]);
    InitLink(links[index + 1], play, outputs[index], quits[index], quit_flows[index]);
    Append(tree.links, links[index + 1]);
  }

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(program, nullptr);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  const LN_Instruction *play_instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnInit), LN_OpCode::PlayAction);
  ASSERT_NE(play_instruction, nullptr);
  EXPECT_EQ(uint8_t(play_instruction->secondary_int_value), LN_ACTION_OUTPUT_ALL);
  ASSERT_LT(play_instruction->property_ref_index, program->GetBoolExpressions().size());
  EXPECT_EQ(program->GetBoolExpressions()[play_instruction->property_ref_index].kind,
            LN_BoolExpressionKind::ActionLifecycleEvent);

  const std::array<LN_BoolExpressionKind, 4> result_kinds = {
      LN_BoolExpressionKind::ActionStarted,
      LN_BoolExpressionKind::ActionCompleted,
      LN_BoolExpressionKind::ActionInterrupted,
      LN_BoolExpressionKind::ActionFailed};
  for (const LN_BoolExpressionKind kind : result_kinds) {
    EXPECT_TRUE(std::any_of(program->GetBoolExpressions().begin(),
                            program->GetBoolExpressions().end(),
                            [kind, play_instruction](const LN_BoolExpression &expression) {
                              return expression.kind == kind &&
                                     expression.input0 == play_instruction->property_ref_index;
                            }));
  }
  EXPECT_EQ(std::count_if(program->GetInstructions(LN_Event::OnFixedUpdate).begin(),
                          program->GetInstructions(LN_Event::OnFixedUpdate).end(),
                          [](const LN_Instruction &instruction) {
                            return instruction.opcode == LN_OpCode::QuitGame;
                          }),
            4);
}

TEST(LN_TreeCompiler, CompilesSetShapeKeyValueInputs)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueString name_default = {};
  SetCString(name_default.value, "Smile");
  blender::bNodeSocketValueFloat value_default = {};
  value_default.value = 0.625f;
  blender::bNode set_shape_key = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket object = {};
  blender::bNodeSocket name = {};
  blender::bNodeSocket value = {};
  blender::bNodeSocket done = {};
  InitNode(set_shape_key, "LogicNativeSetShapeKeyValue", "Set Shape Key Value", 2);
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(name, "Shape Key", blender::SOCK_STRING, &name_default);
  InitSocket(value, "Value", blender::SOCK_FLOAT, &value_default);
  InitSocket(done, "Done", blender::SOCK_BOOLEAN, nullptr);
  Append(set_shape_key.inputs, flow);
  Append(set_shape_key.inputs, object);
  Append(set_shape_key.inputs, name);
  Append(set_shape_key.inputs, value);
  Append(set_shape_key.outputs, done);
  Append(tree.nodes, set_shape_key);

  blender::bNodeLink flow_link = {};
  InitLink(flow_link, on_update, on_update_pulse, set_shape_key, flow);
  Append(tree.links, flow_link);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  const LN_Instruction *instruction = FindInstruction(program->GetInstructions(LN_Event::OnUpdate),
                                                      LN_OpCode::SetShapeKeyValue);
  ASSERT_NE(instruction, nullptr);
  EXPECT_EQ(instruction->value_expr_index, LN_INVALID_INDEX);
  const LN_StringExpression *name_expression = StringExpressionAt(*program,
                                                                  instruction->string_expr_index);
  ASSERT_NE(name_expression, nullptr);
  EXPECT_EQ(name_expression->kind, LN_StringExpressionKind::Constant);
  EXPECT_EQ(name_expression->string_value, "Smile");
  const LN_FloatExpression *value_expression = FloatExpressionAt(*program,
                                                                 instruction->float_expr_index);
  ASSERT_NE(value_expression, nullptr);
  EXPECT_EQ(value_expression->kind, LN_FloatExpressionKind::Constant);
  EXPECT_EQ(value_expression->float_value, 0.625f);
}

TEST(LN_TreeCompiler, OrdersAddObjectResultConsumersAfterEachAddObject)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::Object template_object = {};
  blender::Object transform_object = {};
  SetCString(template_object.id.name, "OBTemplate");
  SetCString(transform_object.id.name, "OBTransform");

  blender::bNode on_init = {};
  blender::bNodeSocket on_init_pulse = {};
  InitNode(on_init, "LogicNativeOnInit", "On Init", 1);
  InitSocket(on_init_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_init.outputs, on_init_pulse);
  Append(tree.nodes, on_init);

  blender::bNodeSocketValueObject template_default = {};
  blender::bNodeSocketValueObject transform_default = {};
  blender::bNodeSocketValueFloat life_default = {};
  blender::bNodeSocketValueBoolean full_copy_default = {};
  blender::bNodeSocketValueBoolean own_mesh_default = {};
  template_default.value = &template_object;
  transform_default.value = &transform_object;
  life_default.value = 0.0f;
  full_copy_default.value = 0;
  own_mesh_default.value = 0;

  auto init_add_object_node = [&](blender::bNode &node,
                                  blender::bNodeSocket &flow,
                                  blender::bNodeSocket &object_to_add,
                                  blender::bNodeSocket &copy_transform,
                                  blender::bNodeSocket &life,
                                  blender::bNodeSocket &full_copy,
                                  blender::bNodeSocket &own_mesh,
                                  blender::bNodeSocket &done,
                                  blender::bNodeSocket &added_object,
                                  const char *name,
                                  int32_t identifier) {
    InitNode(node, "LogicNativeAddObject", name, identifier);
    InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
    InitSocket(object_to_add, "Object to Add", blender::SOCK_OBJECT, &template_default);
    InitSocket(copy_transform, "Copy Transform", blender::SOCK_OBJECT, &transform_default);
    InitSocket(life, "Life", blender::SOCK_FLOAT, &life_default);
    InitSocket(full_copy, "Full Copy", blender::SOCK_BOOLEAN, &full_copy_default);
    InitSocket(own_mesh, "Own Mesh", blender::SOCK_BOOLEAN, &own_mesh_default);
    InitSocket(done, "Done", blender::SOCK_BOOLEAN, nullptr);
    InitSocket(added_object, "Added Object", blender::SOCK_OBJECT, nullptr);
    SetCString(done.identifier, "Out");
    SetCString(added_object.identifier, "Object");
    Append(node.inputs, flow);
    Append(node.inputs, object_to_add);
    Append(node.inputs, copy_transform);
    Append(node.inputs, life);
    Append(node.inputs, full_copy);
    Append(node.inputs, own_mesh);
    Append(node.outputs, done);
    Append(node.outputs, added_object);
    Append(tree.nodes, node);
  };

  blender::bNode add_object_a = {};
  blender::bNodeSocket add_a_flow = {};
  blender::bNodeSocket add_a_object = {};
  blender::bNodeSocket add_a_transform = {};
  blender::bNodeSocket add_a_life = {};
  blender::bNodeSocket add_a_full_copy = {};
  blender::bNodeSocket add_a_own_mesh = {};
  blender::bNodeSocket add_a_done = {};
  blender::bNodeSocket add_a_added_object = {};
  init_add_object_node(add_object_a,
                       add_a_flow,
                       add_a_object,
                       add_a_transform,
                       add_a_life,
                       add_a_full_copy,
                       add_a_own_mesh,
                       add_a_done,
                       add_a_added_object,
                       "Add Object A",
                       2);

  blender::bNode add_object_b = {};
  blender::bNodeSocket add_b_flow = {};
  blender::bNodeSocket add_b_object = {};
  blender::bNodeSocket add_b_transform = {};
  blender::bNodeSocket add_b_life = {};
  blender::bNodeSocket add_b_full_copy = {};
  blender::bNodeSocket add_b_own_mesh = {};
  blender::bNodeSocket add_b_done = {};
  blender::bNodeSocket add_b_added_object = {};
  init_add_object_node(add_object_b,
                       add_b_flow,
                       add_b_object,
                       add_b_transform,
                       add_b_life,
                       add_b_full_copy,
                       add_b_own_mesh,
                       add_b_done,
                       add_b_added_object,
                       "Add Object B",
                       3);

  auto init_get_id_node = [&](blender::bNode &node,
                              blender::bNodeSocket &object,
                              blender::bNodeSocket &id,
                              const char *name,
                              int32_t identifier) {
    InitNode(node, "LogicNativeGetObjectID", name, identifier);
    InitSocket(object, "Object", blender::SOCK_OBJECT, nullptr);
    InitSocket(id, "ID", blender::SOCK_STRING, nullptr);
    Append(node.inputs, object);
    Append(node.outputs, id);
    Append(tree.nodes, node);
  };

  blender::bNode get_id_a = {};
  blender::bNodeSocket get_id_a_object = {};
  blender::bNodeSocket get_id_a_id = {};
  init_get_id_node(get_id_a, get_id_a_object, get_id_a_id, "Get ID A", 4);

  blender::bNode get_id_b = {};
  blender::bNodeSocket get_id_b_object = {};
  blender::bNodeSocket get_id_b_id = {};
  init_get_id_node(get_id_b, get_id_b_object, get_id_b_id, "Get ID B", 5);

  auto init_set_string_node = [&](blender::bNode &node,
                                  blender::bNodeSocket &flow,
                                  blender::bNodeSocket &object,
                                  blender::bNodeSocket &property,
                                  blender::bNodeSocket &value,
                                  blender::bNodeSocket &done,
                                  blender::bNodeSocketValueString &property_default,
                                  const char *name,
                                  const char *property_name,
                                  int32_t identifier) {
    SetCString(property_default.value, property_name);
    InitNode(node, "LogicNativeSetGamePropertyString", name, identifier);
    InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
    InitSocket(object, "Object", blender::SOCK_OBJECT, nullptr);
    InitSocket(property, "Property", blender::SOCK_STRING, &property_default);
    InitSocket(value, "Value", blender::SOCK_STRING, nullptr);
    InitSocket(done, "Done", blender::SOCK_BOOLEAN, nullptr);
    Append(node.inputs, flow);
    Append(node.inputs, object);
    Append(node.inputs, property);
    Append(node.inputs, value);
    Append(node.outputs, done);
    Append(tree.nodes, node);
  };

  blender::bNodeSocketValueString property_a_default = {};
  blender::bNode set_id_a = {};
  blender::bNodeSocket set_id_a_flow = {};
  blender::bNodeSocket set_id_a_object = {};
  blender::bNodeSocket set_id_a_property = {};
  blender::bNodeSocket set_id_a_value = {};
  blender::bNodeSocket set_id_a_done = {};
  init_set_string_node(set_id_a,
                       set_id_a_flow,
                       set_id_a_object,
                       set_id_a_property,
                       set_id_a_value,
                       set_id_a_done,
                       property_a_default,
                       "Set ID A",
                       "added_object_id_a",
                       6);

  blender::bNodeSocketValueString property_b_default = {};
  blender::bNode set_id_b = {};
  blender::bNodeSocket set_id_b_flow = {};
  blender::bNodeSocket set_id_b_object = {};
  blender::bNodeSocket set_id_b_property = {};
  blender::bNodeSocket set_id_b_value = {};
  blender::bNodeSocket set_id_b_done = {};
  init_set_string_node(set_id_b,
                       set_id_b_flow,
                       set_id_b_object,
                       set_id_b_property,
                       set_id_b_value,
                       set_id_b_done,
                       property_b_default,
                       "Set ID B",
                       "added_object_id_b",
                       7);

  blender::bNodeLink init_to_add_a = {};
  blender::bNodeLink add_a_done_to_set_a = {};
  blender::bNodeLink add_a_object_to_get_a = {};
  blender::bNodeLink get_a_to_set_a = {};
  blender::bNodeLink set_a_done_to_add_b = {};
  blender::bNodeLink add_b_done_to_set_b = {};
  blender::bNodeLink add_b_object_to_get_b = {};
  blender::bNodeLink get_b_to_set_b = {};
  InitLink(init_to_add_a, on_init, on_init_pulse, add_object_a, add_a_flow);
  InitLink(add_a_done_to_set_a, add_object_a, add_a_done, set_id_a, set_id_a_flow);
  InitLink(add_a_object_to_get_a, add_object_a, add_a_added_object, get_id_a, get_id_a_object);
  InitLink(get_a_to_set_a, get_id_a, get_id_a_id, set_id_a, set_id_a_value);
  InitLink(set_a_done_to_add_b, set_id_a, set_id_a_done, add_object_b, add_b_flow);
  InitLink(add_b_done_to_set_b, add_object_b, add_b_done, set_id_b, set_id_b_flow);
  InitLink(add_b_object_to_get_b, add_object_b, add_b_added_object, get_id_b, get_id_b_object);
  InitLink(get_b_to_set_b, get_id_b, get_id_b_id, set_id_b, set_id_b_value);
  Append(tree.links, init_to_add_a);
  Append(tree.links, add_a_done_to_set_a);
  Append(tree.links, add_a_object_to_get_a);
  Append(tree.links, get_a_to_set_a);
  Append(tree.links, set_a_done_to_add_b);
  Append(tree.links, add_b_done_to_set_b);
  Append(tree.links, add_b_object_to_get_b);
  Append(tree.links, get_b_to_set_b);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());

  const std::vector<LN_Instruction> &instructions = program->GetInstructions(LN_Event::OnInit);
  std::string opcode_sequence;
  for (const LN_Instruction &instruction : instructions) {
    if (!opcode_sequence.empty()) {
      opcode_sequence += ", ";
    }
    opcode_sequence += std::to_string(int(instruction.opcode));
    if (instruction.source_ref_index < program->GetSourceRefs().size()) {
      opcode_sequence += ":";
      opcode_sequence += program->GetSourceRefs()[instruction.source_ref_index].node_name;
    }
  }
  ASSERT_EQ(instructions.size(), 8u) << opcode_sequence;
  EXPECT_EQ(instructions[0].opcode, LN_OpCode::Nop);
  EXPECT_EQ(instructions[1].opcode, LN_OpCode::AddObject);
  EXPECT_EQ(instructions[2].opcode, LN_OpCode::BranchRoute);
  EXPECT_EQ(instructions[3].opcode, LN_OpCode::SetGameProperty);
  EXPECT_EQ(instructions[4].opcode, LN_OpCode::BranchRoute);
  EXPECT_EQ(instructions[5].opcode, LN_OpCode::AddObject);
  EXPECT_EQ(instructions[6].opcode, LN_OpCode::BranchRoute);
  EXPECT_EQ(instructions[7].opcode, LN_OpCode::SetGameProperty);
  EXPECT_NE(instructions[1].property_ref_index, LN_INVALID_INDEX);
  EXPECT_NE(instructions[5].property_ref_index, LN_INVALID_INDEX);

  const std::vector<LN_SourceRef> &source_refs = program->GetSourceRefs();
  ASSERT_LT(instructions[1].source_ref_index, source_refs.size());
  ASSERT_LT(instructions[3].source_ref_index, source_refs.size());
  ASSERT_LT(instructions[5].source_ref_index, source_refs.size());
  ASSERT_LT(instructions[7].source_ref_index, source_refs.size());
  EXPECT_EQ(source_refs[instructions[1].source_ref_index].node_name, "Add Object A");
  EXPECT_EQ(source_refs[instructions[3].source_ref_index].node_name, "Set ID A");
  EXPECT_EQ(source_refs[instructions[5].source_ref_index].node_name, "Add Object B");
  EXPECT_EQ(source_refs[instructions[7].source_ref_index].node_name, "Set ID B");
}

TEST(LN_TreeCompiler, AddObjectDoesNotStoreUnusedAddedObjectOutput)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::Object template_object = {};
  blender::Object transform_object = {};
  SetCString(template_object.id.name, "OBTemplate");
  SetCString(transform_object.id.name, "OBTransform");

  blender::bNode on_init = {};
  blender::bNodeSocket on_init_pulse = {};
  InitNode(on_init, "LogicNativeOnInit", "On Init", 1);
  InitSocket(on_init_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_init.outputs, on_init_pulse);
  Append(tree.nodes, on_init);

  blender::bNodeSocketValueObject template_default = {};
  blender::bNodeSocketValueObject transform_default = {};
  blender::bNodeSocketValueFloat life_default = {};
  blender::bNodeSocketValueBoolean full_copy_default = {};
  blender::bNodeSocketValueBoolean own_mesh_default = {};
  template_default.value = &template_object;
  transform_default.value = &transform_object;
  life_default.value = 0.0f;
  full_copy_default.value = 0;
  own_mesh_default.value = 0;

  blender::bNode add_object = {};
  blender::bNodeSocket add_flow = {};
  blender::bNodeSocket add_object_to_add = {};
  blender::bNodeSocket add_copy_transform = {};
  blender::bNodeSocket add_life = {};
  blender::bNodeSocket add_full_copy = {};
  blender::bNodeSocket add_own_mesh = {};
  blender::bNodeSocket add_done = {};
  blender::bNodeSocket add_added_object = {};
  InitNode(add_object, "LogicNativeAddObject", "Add Object", 2);
  InitSocket(add_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(add_object_to_add, "Object to Add", blender::SOCK_OBJECT, &template_default);
  InitSocket(add_copy_transform, "Copy Transform", blender::SOCK_OBJECT, &transform_default);
  InitSocket(add_life, "Life", blender::SOCK_FLOAT, &life_default);
  InitSocket(add_full_copy, "Full Copy", blender::SOCK_BOOLEAN, &full_copy_default);
  InitSocket(add_own_mesh, "Own Mesh", blender::SOCK_BOOLEAN, &own_mesh_default);
  InitSocket(add_done, "Done", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(add_added_object, "Added Object", blender::SOCK_OBJECT, nullptr);
  SetCString(add_done.identifier, "Out");
  SetCString(add_added_object.identifier, "Object");
  Append(add_object.inputs, add_flow);
  Append(add_object.inputs, add_object_to_add);
  Append(add_object.inputs, add_copy_transform);
  Append(add_object.inputs, add_life);
  Append(add_object.inputs, add_full_copy);
  Append(add_object.inputs, add_own_mesh);
  Append(add_object.outputs, add_done);
  Append(add_object.outputs, add_added_object);
  Append(tree.nodes, add_object);

  blender::bNodeLink init_to_add = {};
  InitLink(init_to_add, on_init, on_init_pulse, add_object, add_flow);
  Append(tree.links, init_to_add);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());

  const std::vector<LN_Instruction> &instructions = program->GetInstructions(LN_Event::OnInit);
  ASSERT_EQ(instructions.size(), 2u);
  EXPECT_EQ(instructions[1].opcode, LN_OpCode::AddObject);
  EXPECT_EQ(instructions[1].property_ref_index, LN_INVALID_INDEX);
  EXPECT_TRUE(program->GetTreePropertyRefs().empty());
}

TEST(LN_TreeCompiler, EmitsCommandsChainedFromStopAllSoundsDoneUsesConditionDriver)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnFixedUpdate", "On Fixed Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueBoolean condition_default = {};
  condition_default.value = 1;
  blender::bNode stop_sounds = {};
  blender::bNodeSocket stop_condition = {};
  blender::bNodeSocket stop_done = {};
  InitNode(stop_sounds, "LogicNativeStopAllSounds", "Stop All Sounds", 2);
  InitSocket(stop_condition, "Flow", blender::SOCK_BOOLEAN, &condition_default);
  InitSocket(stop_done, "Done", blender::SOCK_BOOLEAN, nullptr);
  Append(stop_sounds.inputs, stop_condition);
  Append(stop_sounds.outputs, stop_done);
  Append(tree.nodes, stop_sounds);

  blender::bNodeSocketValueVector position_default = {};
  position_default.value[0] = 9.0f;
  position_default.value[1] = 8.0f;
  position_default.value[2] = 7.0f;
  blender::bNode set_position = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket position = {};
  SetObjectAttributeAuxSockets set_attribute_aux = {};
  InitSetObjectAttributeVectorNode(
      set_position, set_attribute_aux, flow, position, "Set Position", 3, 0, &position_default);
  Append(tree.nodes, set_position);

  blender::bNodeLink event_to_stop = {};
  blender::bNodeLink done_to_position = {};
  InitLink(event_to_stop, on_update, on_update_pulse, stop_sounds, stop_condition);
  InitLink(done_to_position, stop_sounds, stop_done, set_position, flow);
  Append(tree.links, event_to_stop);
  Append(tree.links, done_to_position);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors());
  EXPECT_NE(
      nullptr,
      FindInstruction(program->GetInstructions(LN_Event::OnFixedUpdate),
                      LN_OpCode::StopAllSounds));
  EXPECT_TRUE(HasVectorInstruction(program->GetInstructions(LN_Event::OnFixedUpdate),
                                   LN_OpCode::SetTransformVector,
                                   MT_Vector3(9.0f, 8.0f, 7.0f)));
}

TEST(LN_TreeCompiler, EmitsSetWorldPositionThroughConstantBranch)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnFixedUpdate", "On Fixed Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueBoolean condition_default = {};
  condition_default.value = 1;
  blender::bNode branch = {};
  blender::bNodeSocket branch_flow = {};
  blender::bNodeSocket condition = {};
  blender::bNodeSocket true_output = {};
  blender::bNodeSocket false_output = {};
  InitNode(branch, "LogicNativeBranch", "Branch", 2);
  InitSocket(branch_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(condition, "Condition", blender::SOCK_BOOLEAN, &condition_default);
  InitSocket(true_output, "True", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(false_output, "False", blender::SOCK_BOOLEAN, nullptr);
  Append(branch.inputs, branch_flow);
  Append(branch.inputs, condition);
  Append(branch.outputs, true_output);
  Append(branch.outputs, false_output);
  Append(tree.nodes, branch);

  blender::bNodeSocketValueVector position_default = {};
  position_default.value[0] = -1.0f;
  position_default.value[1] = 4.0f;
  position_default.value[2] = 0.5f;
  blender::bNode set_position = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket position = {};
  SetObjectAttributeAuxSockets set_attribute_aux = {};
  InitSetObjectAttributeVectorNode(
      set_position, set_attribute_aux, flow, position, "Set Position", 3, 0, &position_default);
  Append(tree.nodes, set_position);

  blender::bNodeLink event_to_branch = {};
  blender::bNodeLink branch_to_set_position = {};
  InitLink(event_to_branch, on_update, on_update_pulse, branch, branch_flow);
  InitLink(branch_to_set_position, branch, true_output, set_position, flow);
  Append(tree.links, event_to_branch);
  Append(tree.links, branch_to_set_position);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors());
  const std::vector<LN_Instruction> &instructions = program->GetInstructions(
      LN_Event::OnFixedUpdate);
  const int route_index = FindInstructionIndex(instructions, LN_OpCode::BranchRoute);
  const int set_index = FindInstructionIndex(instructions, LN_OpCode::SetTransformVector);
  ASSERT_GE(route_index, 0);
  ASSERT_GE(set_index, 0);
  EXPECT_LT(route_index, set_index);
  EXPECT_TRUE(HasVectorInstruction(
      instructions, LN_OpCode::SetTransformVector, MT_Vector3(-1.0f, 4.0f, 0.5f)));

  const LN_Instruction &route_instruction = instructions[size_t(route_index)];
  EXPECT_TRUE(route_instruction.bool_value);
  const LN_BoolExpression *route_condition = BoolExpressionAt(*program,
                                                              route_instruction.bool_expr_index);
  ASSERT_NE(route_condition, nullptr);
  EXPECT_EQ(route_condition->kind, LN_BoolExpressionKind::Constant);
  EXPECT_TRUE(route_condition->bool_value);

  const LN_Instruction &set_instruction = instructions[size_t(set_index)];
  const LN_BoolExpression *guard = BoolExpressionAt(*program,
                                                    set_instruction.bool_guard_expr_index);
  ASSERT_NE(guard, nullptr);
  EXPECT_EQ(guard->kind, LN_BoolExpressionKind::InstructionExecuted);
  EXPECT_EQ(guard->input0, uint32_t(route_index));
}

TEST(LN_TreeCompiler, ObjectsCollidingBlankObjectAUsesOwnerFallbackInBranchCondition)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::Object plane_object = {};
  SetCString(plane_object.id.name, "OBPlane");

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnFixedUpdate", "On Fixed Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueObject object_a_default = {};
  blender::bNodeSocketValueObject object_b_default = {};
  object_b_default.value = &plane_object;
  blender::bNode objects_colliding = {};
  blender::bNodeSocket object_a = {};
  blender::bNodeSocket object_b = {};
  blender::bNodeSocket colliding = {};
  InitNode(objects_colliding, "LogicNativeObjectsColliding", "Objects Colliding", 2);
  InitSocket(object_a, "Object A", blender::SOCK_OBJECT, &object_a_default);
  InitSocket(object_b, "Object B", blender::SOCK_OBJECT, &object_b_default);
  InitSocket(colliding, "Colliding", blender::SOCK_BOOLEAN, nullptr);
  Append(objects_colliding.inputs, object_a);
  Append(objects_colliding.inputs, object_b);
  Append(objects_colliding.outputs, colliding);
  Append(tree.nodes, objects_colliding);

  blender::bNode branch = {};
  blender::bNodeSocket branch_flow = {};
  blender::bNodeSocket condition = {};
  blender::bNodeSocket true_output = {};
  blender::bNodeSocket false_output = {};
  InitNode(branch, "LogicNativeBranch", "Branch", 3);
  InitSocket(branch_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(condition, "Condition", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(true_output, "True", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(false_output, "False", blender::SOCK_BOOLEAN, nullptr);
  Append(branch.inputs, branch_flow);
  Append(branch.inputs, condition);
  Append(branch.outputs, true_output);
  Append(branch.outputs, false_output);
  Append(tree.nodes, branch);

  blender::bNodeSocketValueVector position_default = {};
  position_default.value[0] = 1.0f;
  position_default.value[1] = 2.0f;
  position_default.value[2] = 3.0f;
  blender::bNode set_position = {};
  blender::bNodeSocket set_flow = {};
  blender::bNodeSocket position = {};
  SetObjectAttributeAuxSockets set_attribute_aux = {};
  InitSetObjectAttributeVectorNode(set_position,
                                   set_attribute_aux,
                                   set_flow,
                                   position,
                                   "Set Position",
                                   4,
                                   0,
                                   &position_default);
  Append(tree.nodes, set_position);

  blender::bNodeLink event_to_branch = {};
  blender::bNodeLink colliding_to_condition = {};
  blender::bNodeLink branch_to_set_position = {};
  InitLink(event_to_branch, on_update, on_update_pulse, branch, branch_flow);
  InitLink(colliding_to_condition, objects_colliding, colliding, branch, condition);
  InitLink(branch_to_set_position, branch, true_output, set_position, set_flow);
  Append(tree.links, event_to_branch);
  Append(tree.links, colliding_to_condition);
  Append(tree.links, branch_to_set_position);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());

  const std::vector<LN_Instruction> &instructions = program->GetInstructions(
      LN_Event::OnFixedUpdate);
  const int route_index = FindInstructionIndex(instructions, LN_OpCode::BranchRoute);
  const int set_index = FindInstructionIndex(instructions, LN_OpCode::SetTransformVector);
  ASSERT_GE(route_index, 0);
  ASSERT_GE(set_index, 0);

  const LN_Instruction &route_instruction = instructions[size_t(route_index)];
  EXPECT_TRUE(route_instruction.bool_value);
  const LN_BoolExpression *route_condition = BoolExpressionAt(*program,
                                                              route_instruction.bool_expr_index);
  ASSERT_NE(route_condition, nullptr);
  EXPECT_EQ(route_condition->kind, LN_BoolExpressionKind::ObjectsColliding);
  EXPECT_EQ(route_condition->input0, LN_INVALID_INDEX);
  EXPECT_NE(route_condition->input1, LN_INVALID_INDEX);

  const LN_Instruction &set_instruction = instructions[size_t(set_index)];
  const LN_BoolExpression *guard = BoolExpressionAt(*program,
                                                    set_instruction.bool_guard_expr_index);
  ASSERT_NE(guard, nullptr);
  EXPECT_EQ(guard->kind, LN_BoolExpressionKind::InstructionExecuted);
  EXPECT_EQ(guard->input0, uint32_t(route_index));
}

TEST(LN_TreeCompiler,
     MissingAssignMaterialToSlotInputSkipsNodeWithoutDisablingSiblingBranchCommand)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnFixedUpdate", "On Fixed Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueBoolean condition_default = {};
  condition_default.value = 1;
  blender::bNode branch = {};
  blender::bNodeSocket branch_flow = {};
  blender::bNodeSocket condition = {};
  blender::bNodeSocket true_output = {};
  blender::bNodeSocket false_output = {};
  InitNode(branch, "LogicNativeBranch", "Branch", 2);
  InitSocket(branch_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(condition, "Condition", blender::SOCK_BOOLEAN, &condition_default);
  InitSocket(true_output, "True", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(false_output, "False", blender::SOCK_BOOLEAN, nullptr);
  Append(branch.inputs, branch_flow);
  Append(branch.inputs, condition);
  Append(branch.outputs, true_output);
  Append(branch.outputs, false_output);
  Append(tree.nodes, branch);

  blender::bNodeSocketValueVector torque_default = {};
  torque_default.value[2] = 7.0f;
  blender::bNodeSocketValueBoolean local_default = {};
  local_default.value = 1;
  blender::bNode apply_torque = {};
  blender::bNodeSocket torque_flow = {};
  blender::bNodeSocket torque_object = {};
  blender::bNodeSocket torque = {};
  blender::bNodeSocket local = {};
  InitNode(apply_torque, "LogicNativeApplyTorque", "Apply Torque", 3);
  InitSocket(torque_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(torque_object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(torque, "Torque", blender::SOCK_VECTOR, &torque_default);
  InitSocket(local, "Local", blender::SOCK_BOOLEAN, &local_default);
  Append(apply_torque.inputs, torque_flow);
  Append(apply_torque.inputs, torque_object);
  Append(apply_torque.inputs, torque);
  Append(apply_torque.inputs, local);
  Append(tree.nodes, apply_torque);

  blender::bNodeSocketValueInt slot_default = {};
  slot_default.value = 1;
  blender::bNode set_material = {};
  blender::bNodeSocket material_flow = {};
  blender::bNodeSocket material_object = {};
  blender::bNodeSocketValueMaterial material_default = {};
  blender::bNodeSocket material = {};
  blender::bNodeSocket slot_socket = {};
  InitNode(set_material, "LogicNativeSetMaterialSlot", "Assign Material To Slot", 4);
  InitSocket(material_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(material_object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(material, "Material", blender::SOCK_MATERIAL, &material_default);
  InitSocket(slot_socket, "Slot", blender::SOCK_INT, &slot_default);
  Append(set_material.inputs, material_flow);
  Append(set_material.inputs, material_object);
  Append(set_material.inputs, material);
  Append(set_material.inputs, slot_socket);
  Append(tree.nodes, set_material);

  blender::bNodeLink event_to_branch = {};
  blender::bNodeLink branch_to_torque = {};
  blender::bNodeLink branch_to_material = {};
  InitLink(event_to_branch, on_update, on_update_pulse, branch, branch_flow);
  InitLink(branch_to_torque, branch, true_output, apply_torque, torque_flow);
  InitLink(branch_to_material, branch, true_output, set_material, material_flow);
  Append(tree.links, event_to_branch);
  Append(tree.links, branch_to_torque);
  Append(tree.links, branch_to_material);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  EXPECT_TRUE(
      HasWarningContaining(program->GetCompileReport(), "Assign Material To Slot skipped"));

  const std::vector<LN_Instruction> &instructions = program->GetInstructions(
      LN_Event::OnFixedUpdate);
  const int route_index = FindInstructionIndex(instructions, LN_OpCode::BranchRoute);
  const int torque_index = FindInstructionIndex(instructions, LN_OpCode::ApplyPhysicsVector);
  EXPECT_EQ(FindInstruction(instructions, LN_OpCode::SetMaterialSlot), nullptr);
  ASSERT_GE(route_index, 0);
  ASSERT_GE(torque_index, 0);

  const LN_Instruction &torque_instruction = instructions[size_t(torque_index)];
  EXPECT_EQ(torque_instruction.vector_operation_channel,
            uint8_t(LN_VectorOperationChannel::Torque));
  const LN_BoolExpression *guard = BoolExpressionAt(*program,
                                                    torque_instruction.bool_guard_expr_index);
  ASSERT_NE(guard, nullptr);
  EXPECT_EQ(guard->kind, LN_BoolExpressionKind::InstructionExecuted);
  EXPECT_EQ(guard->input0, uint32_t(route_index));
}

TEST(LN_TreeCompiler, SetMaterialParameterCompilesColorToSelectedMaterialNodeSocket)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueString node_name_default = {};
  SetCString(node_name_default.value, "Principled BSDF");
  blender::bNodeSocketValueString socket_name_default = {};
  SetCString(socket_name_default.value, "Base Color");
  blender::bNodeSocketValueRGBA color_default = {};
  color_default.value[0] = 0.25f;
  color_default.value[1] = 0.5f;
  color_default.value[2] = 0.75f;
  color_default.value[3] = 1.0f;
  blender::bNodeSocketValueInt slot_default = {};
  slot_default.value = 2;

  blender::bNode set_parameter = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket object = {};
  blender::bNodeSocket slot_socket = {};
  blender::bNodeSocket material = {};
  blender::bNodeSocket node_name = {};
  blender::bNodeSocket socket_name = {};
  MaterialParameterValueSockets value_sockets;
  InitNode(set_parameter, "LogicNativeSetMaterialParameter", "Set Material Parameter", 2);
  set_parameter.custom1 = 4;
  set_parameter.custom2 = 1;
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(slot_socket, "Slot", blender::SOCK_INT, &slot_default);
  InitSocket(material, "Material", blender::SOCK_MATERIAL, nullptr);
  InitSocket(node_name, "Node Name", blender::SOCK_STRING, &node_name_default);
  InitSocket(socket_name, "Socket", blender::SOCK_STRING, &socket_name_default);
  Append(set_parameter.inputs, flow);
  Append(set_parameter.inputs, object);
  Append(set_parameter.inputs, slot_socket);
  Append(set_parameter.inputs, material);
  Append(set_parameter.inputs, node_name);
  Append(set_parameter.inputs, socket_name);
  AppendMaterialParameterValueSockets(set_parameter, value_sockets, nullptr, &color_default);
  Append(tree.nodes, set_parameter);

  blender::bNodeLink flow_link = {};
  InitLink(flow_link, on_update, on_update_pulse, set_parameter, flow);
  Append(tree.links, flow_link);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  const LN_Instruction *instruction = FindInstruction(program->GetInstructions(LN_Event::OnUpdate),
                                                      LN_OpCode::SetMaterialParameter);
  ASSERT_NE(instruction, nullptr);

  const LN_StringExpression *compiled_node_name = StringExpressionAt(
      *program, instruction->string_expr_index);
  ASSERT_NE(compiled_node_name, nullptr);
  EXPECT_EQ(compiled_node_name->kind, LN_StringExpressionKind::Constant);
  EXPECT_EQ(compiled_node_name->string_value, "Principled BSDF");

  const LN_StringExpression *compiled_socket_name = StringExpressionAt(
      *program, instruction->secondary_string_expr_index);
  ASSERT_NE(compiled_socket_name, nullptr);
  EXPECT_EQ(compiled_socket_name->kind, LN_StringExpressionKind::Constant);
  EXPECT_EQ(compiled_socket_name->string_value, "Base Color");

  const LN_IntExpression *slot_expr = IntExpressionAt(*program, instruction->int_expr_index);
  ASSERT_NE(slot_expr, nullptr);
  EXPECT_EQ(slot_expr->kind, LN_IntExpressionKind::Constant);
  EXPECT_EQ(slot_expr->int_value, 2);
  EXPECT_EQ(instruction->value_expr_index, LN_INVALID_INDEX);
  EXPECT_EQ(instruction->tertiary_value_expr_index, LN_INVALID_INDEX);

  const LN_ValueExpression *value_expr = ValueExpressionAt(
      *program, instruction->secondary_value_expr_index);
  ASSERT_NE(value_expr, nullptr);
  EXPECT_EQ(value_expr->kind, LN_ValueExpressionKind::FromColor);
  const LN_ColorExpression *color_expr = ColorExpressionAt(*program, value_expr->input0);
  ASSERT_NE(color_expr, nullptr);
  EXPECT_EQ(color_expr->kind, LN_ColorExpressionKind::Constant);
  EXPECT_NEAR(color_expr->color_value[0], 0.25f, 0.0001f);
  EXPECT_NEAR(color_expr->color_value[1], 0.5f, 0.0001f);
  EXPECT_NEAR(color_expr->color_value[2], 0.75f, 0.0001f);
  EXPECT_NEAR(color_expr->color_value[3], 1.0f, 0.0001f);
}

TEST(LN_TreeCompiler, SetMaterialParameterCanTargetSharedMaterialNodeSocket)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::Material material_data = {};
  InitMaterial(material_data, "RuntimeMaterial");

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueMaterial material_default = {};
  material_default.value = &material_data;
  blender::bNodeSocketValueString node_name_default = {};
  SetCString(node_name_default.value, "Emission");
  blender::bNodeSocketValueString socket_name_default = {};
  SetCString(socket_name_default.value, "Strength");
  blender::bNodeSocketValueFloat float_default = {};
  float_default.value = 3.5f;
  blender::bNodeSocketValueInt slot_default = {};
  slot_default.value = 0;

  blender::bNode set_parameter = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket object = {};
  blender::bNodeSocket slot_socket = {};
  blender::bNodeSocket material = {};
  blender::bNodeSocket node_name = {};
  blender::bNodeSocket socket_name = {};
  MaterialParameterValueSockets value_sockets;
  InitNode(set_parameter, "LogicNativeSetMaterialParameter", "Set Material Parameter", 2);
  set_parameter.custom1 = 0;
  set_parameter.custom2 = 0;
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(slot_socket, "Slot", blender::SOCK_INT, &slot_default);
  InitSocket(material, "Material", blender::SOCK_MATERIAL, &material_default);
  InitSocket(node_name, "Node Name", blender::SOCK_STRING, &node_name_default);
  InitSocket(socket_name, "Socket", blender::SOCK_STRING, &socket_name_default);
  Append(set_parameter.inputs, flow);
  Append(set_parameter.inputs, object);
  Append(set_parameter.inputs, slot_socket);
  Append(set_parameter.inputs, material);
  Append(set_parameter.inputs, node_name);
  Append(set_parameter.inputs, socket_name);
  AppendMaterialParameterValueSockets(set_parameter, value_sockets, &float_default, nullptr);
  Append(tree.nodes, set_parameter);

  blender::bNodeLink flow_link = {};
  InitLink(flow_link, on_update, on_update_pulse, set_parameter, flow);
  Append(tree.links, flow_link);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  const LN_Instruction *instruction = FindInstruction(program->GetInstructions(LN_Event::OnUpdate),
                                                      LN_OpCode::SetMaterialParameter);
  ASSERT_NE(instruction, nullptr);

  const LN_StringExpression *node_name_expr = StringExpressionAt(*program,
                                                                 instruction->string_expr_index);
  ASSERT_NE(node_name_expr, nullptr);
  EXPECT_EQ(node_name_expr->kind, LN_StringExpressionKind::Constant);
  EXPECT_EQ(node_name_expr->string_value, "Emission");

  const LN_StringExpression *compiled_socket_name = StringExpressionAt(
      *program, instruction->secondary_string_expr_index);
  ASSERT_NE(compiled_socket_name, nullptr);
  EXPECT_EQ(compiled_socket_name->kind, LN_StringExpressionKind::Constant);
  EXPECT_EQ(compiled_socket_name->string_value, "Strength");

  EXPECT_EQ(instruction->value_expr_index, LN_INVALID_INDEX);
  const LN_ValueExpression *material_expr = ValueExpressionAt(
      *program, instruction->tertiary_value_expr_index);
  ASSERT_NE(material_expr, nullptr);
  EXPECT_EQ(material_expr->kind, LN_ValueExpressionKind::Constant);
  EXPECT_EQ(material_expr->value.type, LN_ValueType::DatablockRef);
  EXPECT_TRUE(material_expr->value.exists);
  EXPECT_EQ(material_expr->value.reference_name, "RuntimeMaterial");

  const LN_ValueExpression *value_expr = ValueExpressionAt(
      *program, instruction->secondary_value_expr_index);
  ASSERT_NE(value_expr, nullptr);
  EXPECT_EQ(value_expr->kind, LN_ValueExpressionKind::FromFloat);
  const LN_FloatExpression *float_expr = FloatExpressionAt(*program, value_expr->input0);
  ASSERT_NE(float_expr, nullptr);
  EXPECT_EQ(float_expr->kind, LN_FloatExpressionKind::Constant);
  EXPECT_NEAR(float_expr->float_value, 3.5f, 0.0001f);
}

TEST(LN_TreeCompiler, SetMaterialParameterRejectsUnsupportedSocketDefaultTypes)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueString node_name_default = {};
  SetCString(node_name_default.value, "Principled BSDF");
  blender::bNodeSocketValueString socket_name_default = {};
  SetCString(socket_name_default.value, "Name");
  blender::bNodeSocketValueInt slot_default = {};
  slot_default.value = 0;

  blender::bNode set_parameter = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket object = {};
  blender::bNodeSocket slot_socket = {};
  blender::bNodeSocket material = {};
  blender::bNodeSocket node_name = {};
  blender::bNodeSocket socket_name = {};
  MaterialParameterValueSockets value_sockets;
  InitNode(set_parameter, "LogicNativeSetMaterialParameter", "Set Material Parameter", 2);
  set_parameter.custom1 = 5;
  set_parameter.custom2 = 1;
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(slot_socket, "Slot", blender::SOCK_INT, &slot_default);
  InitSocket(material, "Material", blender::SOCK_MATERIAL, nullptr);
  InitSocket(node_name, "Node Name", blender::SOCK_STRING, &node_name_default);
  InitSocket(socket_name, "Socket", blender::SOCK_STRING, &socket_name_default);
  Append(set_parameter.inputs, flow);
  Append(set_parameter.inputs, object);
  Append(set_parameter.inputs, slot_socket);
  Append(set_parameter.inputs, material);
  Append(set_parameter.inputs, node_name);
  Append(set_parameter.inputs, socket_name);
  AppendMaterialParameterValueSockets(set_parameter, value_sockets, nullptr, nullptr);
  Append(tree.nodes, set_parameter);

  blender::bNodeLink flow_link = {};
  InitLink(flow_link, on_update, on_update_pulse, set_parameter, flow);
  Append(tree.links, flow_link);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_TRUE(program->GetCompileReport().HasErrors());
  EXPECT_TRUE(
      HasErrorContaining(program->GetCompileReport(), "Set Material Parameter supports only"))
      << FormatCompileReport(program->GetCompileReport());
}

TEST(LN_TreeCompiler, GeometryNodesInputSetterCompilesModifierTarget)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueString modifier_default = {};
  SetCString(modifier_default.value, "GeometryNodes");
  blender::bNodeSocketValueString input_default = {};
  SetCString(input_default.value, "Socket_2");
  blender::bNodeSocketValueFloat modifier_value_default = {};
  modifier_value_default.value = 2.5f;

  blender::bNode set_modifier_input = {};
  blender::bNodeSocket modifier_flow = {};
  blender::bNodeSocket modifier_object = {};
  blender::bNodeSocket modifier_name = {};
  blender::bNodeSocket input_name = {};
  blender::bNodeSocket modifier_string_value = {};
  blender::bNodeSocket modifier_material_value = {};
  blender::bNodeSocketValueString modifier_string_default = {};
  blender::bNodeSocketValueMaterial modifier_material_default = {};
  MaterialParameterValueSockets modifier_value_sockets;
  InitNode(set_modifier_input, "LogicNativeSetGeometryNodesInput", "Set Geometry Nodes Input", 2);
  set_modifier_input.custom1 = 0;
  InitSocket(modifier_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(modifier_object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(modifier_name, "Modifier", blender::SOCK_STRING, &modifier_default);
  InitSocket(input_name, "Input", blender::SOCK_STRING, &input_default);
  Append(set_modifier_input.inputs, modifier_flow);
  Append(set_modifier_input.inputs, modifier_object);
  Append(set_modifier_input.inputs, modifier_name);
  Append(set_modifier_input.inputs, input_name);
  AppendMaterialParameterValueSockets(
      set_modifier_input, modifier_value_sockets, &modifier_value_default, nullptr);
  InitSocketWithIdentifier(modifier_string_value,
                           "String Value",
                           "Value",
                           blender::SOCK_STRING,
                           &modifier_string_default,
                           "NodeSocketLogicString");
  InitSocketWithIdentifier(modifier_material_value,
                           "Material Value",
                           "Value",
                           blender::SOCK_MATERIAL,
                           &modifier_material_default,
                           "NodeSocketLogicMaterial");
  Append(set_modifier_input.inputs, modifier_string_value);
  Append(set_modifier_input.inputs, modifier_material_value);
  Append(tree.nodes, set_modifier_input);

  blender::bNodeLink modifier_flow_link = {};
  InitLink(modifier_flow_link, on_update, on_update_pulse, set_modifier_input, modifier_flow);
  Append(tree.links, modifier_flow_link);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  const std::vector<LN_Instruction> &instructions = program->GetInstructions(LN_Event::OnUpdate);

  const LN_Instruction *modifier_instruction = FindInstruction(instructions,
                                                               LN_OpCode::SetGeometryNodesInput);
  ASSERT_NE(modifier_instruction, nullptr);
  const LN_StringExpression *compiled_modifier = StringExpressionAt(
      *program, modifier_instruction->string_expr_index);
  const LN_StringExpression *compiled_input = StringExpressionAt(
      *program, modifier_instruction->secondary_string_expr_index);
  ASSERT_NE(compiled_modifier, nullptr);
  ASSERT_NE(compiled_input, nullptr);
  EXPECT_EQ(compiled_modifier->string_value, "GeometryNodes");
  EXPECT_EQ(compiled_input->string_value, "Socket_2");
  EXPECT_EQ(modifier_instruction->tertiary_string_expr_index, LN_INVALID_INDEX);
  EXPECT_EQ(modifier_instruction->value_expr_index, LN_INVALID_INDEX);
  const LN_ValueExpression *modifier_value = ValueExpressionAt(
      *program, modifier_instruction->secondary_value_expr_index);
  ASSERT_NE(modifier_value, nullptr);
  EXPECT_EQ(modifier_value->kind, LN_ValueExpressionKind::FromFloat);
  const LN_FloatExpression *compiled_modifier_value = FloatExpressionAt(*program,
                                                                        modifier_value->input0);
  ASSERT_NE(compiled_modifier_value, nullptr);
  EXPECT_NEAR(compiled_modifier_value->float_value, 2.5f, 0.0001f);
}

TEST(LN_TreeCompiler, EditorNodeValueFeedsUniversalSetterAndTypedConsumers)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueInt slot_default = {};
  blender::bNodeSocketValueString modifier_default = {};
  blender::bNodeSocketValueString node_default = {};
  blender::bNodeSocketValueString socket_default = {};
  SetCString(modifier_default.value, "GeometryNodes");
  SetCString(node_default.value, "Extrude Mesh");
  SetCString(socket_default.value, "Offset Scale");

  blender::bNode getter = {};
  blender::bNodeSocket getter_object = {};
  blender::bNodeSocket getter_slot = {};
  blender::bNodeSocket getter_modifier = {};
  blender::bNodeSocket getter_node = {};
  blender::bNodeSocket getter_socket = {};
  blender::bNodeSocket getter_value = {};
  blender::bNodeSocket getter_found = {};
  InitNode(getter, "LogicNativeGetEditorNodeValue", "Get Editor Node Value", 2);
  getter.custom1 = 2;
  InitSocket(getter_object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(getter_slot, "Slot", blender::SOCK_INT, &slot_default);
  InitSocket(getter_modifier, "Modifier", blender::SOCK_STRING, &modifier_default);
  InitSocketWithIdentifier(getter_node, "Node Name", "Node", blender::SOCK_STRING, &node_default);
  InitSocket(getter_socket, "Socket", blender::SOCK_STRING, &socket_default);
  InitSocket(getter_value, "Value", blender::SOCK_CUSTOM, nullptr);
  InitSocket(getter_found, "Found", blender::SOCK_BOOLEAN, nullptr);
  Append(getter.inputs, getter_object);
  Append(getter.inputs, getter_slot);
  Append(getter.inputs, getter_modifier);
  Append(getter.inputs, getter_node);
  Append(getter.inputs, getter_socket);
  Append(getter.outputs, getter_value);
  Append(getter.outputs, getter_found);
  Append(tree.nodes, getter);

  blender::bNode unique = {};
  blender::bNodeSocket unique_flow = {};
  blender::bNodeSocket unique_object = {};
  blender::bNodeSocket unique_slot = {};
  blender::bNodeSocket unique_modifier = {};
  blender::bNodeSocket unique_done = {};
  InitNode(unique, "LogicNativeMakeNodeTreeUnique", "Make Node Tree Unique", 3);
  unique.custom1 = 2;
  InitSocket(unique_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(unique_object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(unique_slot, "Slot", blender::SOCK_INT, &slot_default);
  InitSocket(unique_modifier, "Modifier", blender::SOCK_STRING, &modifier_default);
  InitSocket(unique_done, "Done", blender::SOCK_BOOLEAN, nullptr);
  Append(unique.inputs, unique_flow);
  Append(unique.inputs, unique_object);
  Append(unique.inputs, unique_slot);
  Append(unique.inputs, unique_modifier);
  Append(unique.outputs, unique_done);
  Append(tree.nodes, unique);

  blender::bNode setter = {};
  blender::bNodeSocket setter_flow = {};
  blender::bNodeSocket setter_object = {};
  blender::bNodeSocket setter_slot = {};
  blender::bNodeSocket setter_modifier = {};
  blender::bNodeSocket setter_node = {};
  blender::bNodeSocket setter_socket = {};
  blender::bNodeSocket setter_string_value = {};
  blender::bNodeSocket setter_material_value = {};
  blender::bNodeSocketValueFloat setter_float_default = {};
  blender::bNodeSocketValueString setter_string_default = {};
  blender::bNodeSocketValueMaterial setter_material_default = {};
  MaterialParameterValueSockets setter_values;
  InitNode(setter, "LogicNativeSetEditorNodeValue", "Set Editor Node Value", 3);
  setter.custom1 = 0;
  setter.custom2 = 2 << 4;
  InitSocket(setter_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(setter_object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(setter_slot, "Slot", blender::SOCK_INT, &slot_default);
  InitSocket(setter_modifier, "Modifier", blender::SOCK_STRING, &modifier_default);
  InitSocketWithIdentifier(setter_node, "Node Name", "Node", blender::SOCK_STRING, &node_default);
  InitSocket(setter_socket, "Socket", blender::SOCK_STRING, &socket_default);
  Append(setter.inputs, setter_flow);
  Append(setter.inputs, setter_object);
  Append(setter.inputs, setter_slot);
  Append(setter.inputs, setter_modifier);
  Append(setter.inputs, setter_node);
  Append(setter.inputs, setter_socket);
  AppendMaterialParameterValueSockets(setter, setter_values, &setter_float_default, nullptr);
  InitSocketWithIdentifier(setter_string_value,
                           "String Value",
                           "Value",
                           blender::SOCK_STRING,
                           &setter_string_default,
                           "NodeSocketLogicString");
  InitSocketWithIdentifier(setter_material_value,
                           "Material Value",
                           "Value",
                           blender::SOCK_MATERIAL,
                           &setter_material_default,
                           "NodeSocketLogicMaterial");
  Append(setter.inputs, setter_string_value);
  Append(setter.inputs, setter_material_value);
  Append(tree.nodes, setter);

  blender::bNodeSocketValueString property_default = {};
  SetCString(property_default.value, "copied_value");
  blender::bNode typed_consumer = {};
  blender::bNodeSocket consumer_flow = {};
  blender::bNodeSocket consumer_object = {};
  blender::bNodeSocket consumer_property = {};
  blender::bNodeSocket consumer_value = {};
  InitNode(typed_consumer, "LogicNativeSetGamePropertyFloat", "Set Game Property", 4);
  InitSocket(consumer_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(consumer_object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(consumer_property, "Property", blender::SOCK_STRING, &property_default);
  InitSocket(consumer_value, "Value", blender::SOCK_FLOAT, nullptr);
  Append(typed_consumer.inputs, consumer_flow);
  Append(typed_consumer.inputs, consumer_object);
  Append(typed_consumer.inputs, consumer_property);
  Append(typed_consumer.inputs, consumer_value);
  Append(tree.nodes, typed_consumer);

  blender::bNodeLink unique_flow_link = {};
  blender::bNodeLink setter_flow_link = {};
  blender::bNodeLink setter_value_link = {};
  blender::bNodeLink consumer_flow_link = {};
  blender::bNodeLink consumer_value_link = {};
  InitLink(unique_flow_link, on_update, on_update_pulse, unique, unique_flow);
  InitLink(setter_flow_link, unique, unique_done, setter, setter_flow);
  InitLink(setter_value_link, getter, getter_value, setter, setter_values.float_value);
  InitLink(consumer_flow_link, on_update, on_update_pulse, typed_consumer, consumer_flow);
  InitLink(consumer_value_link, getter, getter_value, typed_consumer, consumer_value);
  Append(tree.links, unique_flow_link);
  Append(tree.links, setter_flow_link);
  Append(tree.links, setter_value_link);
  Append(tree.links, consumer_flow_link);
  Append(tree.links, consumer_value_link);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  const std::vector<LN_Instruction> &instructions = program->GetInstructions(LN_Event::OnUpdate);

  const LN_Instruction *unique_instruction = FindInstruction(instructions,
                                                             LN_OpCode::MakeNodeTreeUnique);
  ASSERT_NE(unique_instruction, nullptr);
  EXPECT_EQ(unique_instruction->int_value, 2);

  const LN_Instruction *setter_instruction = FindInstruction(
      instructions, LN_OpCode::SetGeometryNodeSocketValue);
  ASSERT_NE(setter_instruction, nullptr);
  const LN_ValueExpression *setter_value = ValueExpressionAt(
      *program, setter_instruction->secondary_value_expr_index);
  ASSERT_NE(setter_value, nullptr);
  EXPECT_EQ(setter_value->kind, LN_ValueExpressionKind::EditorNodeValue);

  const LN_Instruction *consumer_instruction = FindInstruction(instructions,
                                                               LN_OpCode::SetGameProperty);
  ASSERT_NE(consumer_instruction, nullptr);
  const LN_FloatExpression *consumer_float = FloatExpressionAt(
      *program, consumer_instruction->float_expr_index);
  ASSERT_NE(consumer_float, nullptr);
  EXPECT_EQ(consumer_float->kind, LN_FloatExpressionKind::FromGenericValue);
  const LN_ValueExpression *consumer_source = ValueExpressionAt(*program, consumer_float->input0);
  ASSERT_NE(consumer_source, nullptr);
  EXPECT_EQ(consumer_source->kind, LN_ValueExpressionKind::EditorNodeValue);
}

TEST(LN_TreeCompiler, EnableDisableModifierCompilesLastStackTarget)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueString modifier_default = {};
  blender::bNodeSocketValueInt index_default = {};
  blender::bNodeSocketValueInt modifier_id_default = {};
  blender::bNodeSocketValueBoolean enabled_default = {};
  index_default.value = 0;
  enabled_default.value = false;

  blender::bNode modifier_node = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket object = {};
  blender::bNodeSocket modifier = {};
  blender::bNodeSocket index = {};
  blender::bNodeSocket modifier_id = {};
  blender::bNodeSocket enabled = {};
  InitNode(modifier_node, "LogicNativeEnableDisableModifier", "Enable or Disable Modifier", 2);
  modifier_node.custom1 = 1;
  modifier_node.custom2 = 1;
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(modifier, "Modifier", blender::SOCK_STRING, &modifier_default);
  InitSocket(index, "Index", blender::SOCK_INT, &index_default);
  InitSocket(modifier_id, "Modifier ID", blender::SOCK_INT, &modifier_id_default);
  InitSocket(enabled, "Enabled", blender::SOCK_BOOLEAN, &enabled_default);
  Append(modifier_node.inputs, flow);
  Append(modifier_node.inputs, object);
  Append(modifier_node.inputs, modifier);
  Append(modifier_node.inputs, index);
  Append(modifier_node.inputs, modifier_id);
  Append(modifier_node.inputs, enabled);
  Append(tree.nodes, modifier_node);

  blender::bNodeLink flow_link = {};
  InitLink(flow_link, on_update, on_update_pulse, modifier_node, flow);
  Append(tree.links, flow_link);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  const LN_Instruction *instruction = FindInstruction(program->GetInstructions(LN_Event::OnUpdate),
                                                      LN_OpCode::EnableDisableModifier);
  ASSERT_NE(instruction, nullptr);
  EXPECT_EQ(instruction->int_value, 2);
  EXPECT_EQ(instruction->string_expr_index, LN_INVALID_INDEX);
  EXPECT_EQ(instruction->int_expr_index, LN_INVALID_INDEX);
  const LN_BoolExpression *enabled_expr = BoolExpressionAt(*program, instruction->bool_expr_index);
  ASSERT_NE(enabled_expr, nullptr);
  EXPECT_FALSE(enabled_expr->bool_value);
}

TEST(LN_TreeCompiler, AssignGeometryNodesModifierCompilesOperationsAndStableIdHandoff)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::GeometryNodeAssetTraits group_traits = {};
  blender::bNodeTree geometry_group = {};
  InitGeometryModifierGroup(geometry_group, group_traits, "RuntimeGeometry");

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueString append_name_default = {};
  SetCString(append_name_default.value, "Runtime Modifier");
  blender::bNodeSocketValueString empty_modifier_default = {};
  blender::bNodeSocketValueInt append_index_default = {};
  blender::bNodeSocketValueInt append_id_default = {};
  blender::bNode append_node = {};
  blender::bNodeSocket append_flow = {};
  blender::bNodeSocket append_object = {};
  blender::bNodeSocket append_name = {};
  blender::bNodeSocket append_modifier = {};
  blender::bNodeSocket append_index = {};
  blender::bNodeSocket append_id = {};
  blender::bNodeSocket append_done = {};
  blender::bNodeSocket append_id_output = {};
  InitNode(
      append_node, "LogicNativeAssignGeometryNodesModifier", "Append Geometry Nodes Modifier", 2);
  append_node.custom1 = 0;
  append_node.custom2 = 0;
  append_node.id = &geometry_group.id;
  InitSocket(append_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(append_object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(append_name, "Modifier Name", blender::SOCK_STRING, &append_name_default);
  InitSocket(append_modifier, "Modifier", blender::SOCK_STRING, &empty_modifier_default);
  InitSocket(append_index, "Index", blender::SOCK_INT, &append_index_default);
  InitSocket(append_id, "Modifier ID", blender::SOCK_INT, &append_id_default);
  InitSocket(append_done, "Done", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(append_id_output, "Modifier ID", blender::SOCK_INT, nullptr);
  Append(append_node.inputs, append_flow);
  Append(append_node.inputs, append_object);
  Append(append_node.inputs, append_name);
  Append(append_node.inputs, append_modifier);
  Append(append_node.inputs, append_index);
  Append(append_node.inputs, append_id);
  Append(append_node.outputs, append_done);
  Append(append_node.outputs, append_id_output);
  Append(tree.nodes, append_node);

  blender::bNodeSocketValueString insert_name_default = {};
  SetCString(insert_name_default.value, "Inserted Modifier");
  blender::bNodeSocketValueInt insert_index_default = {};
  insert_index_default.value = 2;
  blender::bNodeSocketValueInt insert_id_default = {};
  blender::bNode insert_node = {};
  blender::bNodeSocket insert_flow = {};
  blender::bNodeSocket insert_object = {};
  blender::bNodeSocket insert_name = {};
  blender::bNodeSocket insert_modifier = {};
  blender::bNodeSocket insert_index = {};
  blender::bNodeSocket insert_id = {};
  blender::bNodeSocket insert_done = {};
  blender::bNodeSocket insert_id_output = {};
  InitNode(
      insert_node, "LogicNativeAssignGeometryNodesModifier", "Insert Geometry Nodes Modifier", 3);
  insert_node.custom1 = 1;
  insert_node.custom2 = 0;
  insert_node.id = &geometry_group.id;
  InitSocket(insert_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(insert_object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(insert_name, "Modifier Name", blender::SOCK_STRING, &insert_name_default);
  InitSocket(insert_modifier, "Modifier", blender::SOCK_STRING, &empty_modifier_default);
  InitSocket(insert_index, "Index", blender::SOCK_INT, &insert_index_default);
  InitSocket(insert_id, "Modifier ID", blender::SOCK_INT, &insert_id_default);
  InitSocket(insert_done, "Done", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(insert_id_output, "Modifier ID", blender::SOCK_INT, nullptr);
  Append(insert_node.inputs, insert_flow);
  Append(insert_node.inputs, insert_object);
  Append(insert_node.inputs, insert_name);
  Append(insert_node.inputs, insert_modifier);
  Append(insert_node.inputs, insert_index);
  Append(insert_node.inputs, insert_id);
  Append(insert_node.outputs, insert_done);
  Append(insert_node.outputs, insert_id_output);
  Append(tree.nodes, insert_node);

  blender::bNodeSocketValueString replace_name_default = {};
  blender::bNodeSocketValueInt replace_index_default = {};
  blender::bNodeSocketValueInt replace_id_default = {};
  replace_id_default.value = 77;
  blender::bNode replace_node = {};
  blender::bNodeSocket replace_flow = {};
  blender::bNodeSocket replace_object = {};
  blender::bNodeSocket replace_name = {};
  blender::bNodeSocket replace_modifier = {};
  blender::bNodeSocket replace_index = {};
  blender::bNodeSocket replace_id = {};
  blender::bNodeSocket replace_done = {};
  blender::bNodeSocket replace_id_output = {};
  InitNode(replace_node,
           "LogicNativeAssignGeometryNodesModifier",
           "Replace Geometry Nodes Modifier",
           4);
  replace_node.custom1 = 2;
  replace_node.custom2 = 4;
  replace_node.id = &geometry_group.id;
  InitSocket(replace_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(replace_object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(replace_name, "Modifier Name", blender::SOCK_STRING, &replace_name_default);
  InitSocket(replace_modifier, "Modifier", blender::SOCK_STRING, &empty_modifier_default);
  InitSocket(replace_index, "Index", blender::SOCK_INT, &replace_index_default);
  InitSocket(replace_id, "Modifier ID", blender::SOCK_INT, &replace_id_default);
  InitSocket(replace_done, "Done", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(replace_id_output, "Modifier ID", blender::SOCK_INT, nullptr);
  Append(replace_node.inputs, replace_flow);
  Append(replace_node.inputs, replace_object);
  Append(replace_node.inputs, replace_name);
  Append(replace_node.inputs, replace_modifier);
  Append(replace_node.inputs, replace_index);
  Append(replace_node.inputs, replace_id);
  Append(replace_node.outputs, replace_done);
  Append(replace_node.outputs, replace_id_output);
  Append(tree.nodes, replace_node);

  blender::bNodeSocketValueString enable_name_default = {};
  blender::bNodeSocketValueInt enable_index_default = {};
  blender::bNodeSocketValueInt enable_id_default = {};
  blender::bNodeSocketValueBoolean enabled_default = {};
  enabled_default.value = false;
  blender::bNode enable_node = {};
  blender::bNodeSocket enable_flow = {};
  blender::bNodeSocket enable_object = {};
  blender::bNodeSocket enable_modifier = {};
  blender::bNodeSocket enable_index = {};
  blender::bNodeSocket enable_id = {};
  blender::bNodeSocket enabled = {};
  InitNode(enable_node, "LogicNativeEnableDisableModifier", "Disable Assigned Modifier", 5);
  enable_node.custom1 = 2;
  enable_node.custom2 = 1;
  InitSocket(enable_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(enable_object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(enable_modifier, "Modifier", blender::SOCK_STRING, &enable_name_default);
  InitSocket(enable_index, "Index", blender::SOCK_INT, &enable_index_default);
  InitSocket(enable_id, "Modifier ID", blender::SOCK_INT, &enable_id_default);
  InitSocket(enabled, "Enabled", blender::SOCK_BOOLEAN, &enabled_default);
  Append(enable_node.inputs, enable_flow);
  Append(enable_node.inputs, enable_object);
  Append(enable_node.inputs, enable_modifier);
  Append(enable_node.inputs, enable_index);
  Append(enable_node.inputs, enable_id);
  Append(enable_node.inputs, enabled);
  Append(tree.nodes, enable_node);

  blender::bNodeLink update_to_append = {};
  blender::bNodeLink update_to_insert = {};
  blender::bNodeLink update_to_replace = {};
  blender::bNodeLink append_done_to_enable = {};
  blender::bNodeLink append_id_to_enable = {};
  InitLink(update_to_append, on_update, on_update_pulse, append_node, append_flow);
  InitLink(update_to_insert, on_update, on_update_pulse, insert_node, insert_flow);
  InitLink(update_to_replace, on_update, on_update_pulse, replace_node, replace_flow);
  InitLink(append_done_to_enable, append_node, append_done, enable_node, enable_flow);
  InitLink(append_id_to_enable, append_node, append_id_output, enable_node, enable_id);
  Append(tree.links, update_to_append);
  Append(tree.links, update_to_insert);
  Append(tree.links, update_to_replace);
  Append(tree.links, append_done_to_enable);
  Append(tree.links, append_id_to_enable);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());

  const std::vector<LN_Instruction> &instructions = program->GetInstructions(LN_Event::OnUpdate);
  const LN_Instruction *append_instruction = nullptr;
  const LN_Instruction *insert_instruction = nullptr;
  const LN_Instruction *replace_instruction = nullptr;
  for (const LN_Instruction &instruction : instructions) {
    if (instruction.opcode != LN_OpCode::AssignGeometryNodesModifier) {
      continue;
    }
    if (instruction.int_value == 0) {
      append_instruction = &instruction;
    }
    else if (instruction.int_value == 1) {
      insert_instruction = &instruction;
    }
    else if (instruction.int_value == 2) {
      replace_instruction = &instruction;
    }
  }
  ASSERT_NE(append_instruction, nullptr);
  ASSERT_NE(insert_instruction, nullptr);
  ASSERT_NE(replace_instruction, nullptr);

  const LN_ValueExpression *group_expression = ValueExpressionAt(
      *program, append_instruction->secondary_value_expr_index);
  ASSERT_NE(group_expression, nullptr);
  EXPECT_EQ(group_expression->kind, LN_ValueExpressionKind::Constant);
  EXPECT_EQ(group_expression->value.type, LN_ValueType::DatablockRef);
  EXPECT_EQ(group_expression->value.reference_name, "RuntimeGeometry");
  const LN_StringExpression *append_name_expression = StringExpressionAt(
      *program, append_instruction->string_expr_index);
  ASSERT_NE(append_name_expression, nullptr);
  EXPECT_EQ(append_name_expression->string_value, "Runtime Modifier");
  EXPECT_NE(append_instruction->property_ref_index, LN_INVALID_INDEX);

  const LN_IntExpression *insert_index_expression = IntExpressionAt(
      *program, insert_instruction->int_expr_index);
  ASSERT_NE(insert_index_expression, nullptr);
  EXPECT_EQ(insert_index_expression->kind, LN_IntExpressionKind::Constant);
  EXPECT_EQ(insert_index_expression->int_value, 2);

  EXPECT_EQ(replace_instruction->secondary_int_value, 4);
  const LN_IntExpression *replace_id_expression = IntExpressionAt(
      *program, replace_instruction->int_expr_index);
  ASSERT_NE(replace_id_expression, nullptr);
  EXPECT_EQ(replace_id_expression->kind, LN_IntExpressionKind::Constant);
  EXPECT_EQ(replace_id_expression->int_value, 77);

  const LN_Instruction *enable_instruction = FindInstruction(instructions,
                                                             LN_OpCode::EnableDisableModifier);
  ASSERT_NE(enable_instruction, nullptr);
  EXPECT_EQ(enable_instruction->int_value, 4);
  const LN_IntExpression *enable_id_expression = IntExpressionAt(
      *program, enable_instruction->int_expr_index);
  ASSERT_NE(enable_id_expression, nullptr);
  EXPECT_EQ(enable_id_expression->kind, LN_IntExpressionKind::RuntimeTreeProperty);
  EXPECT_EQ(enable_id_expression->property_ref_index, append_instruction->property_ref_index);
}

TEST(LN_TreeCompiler, AssignGeometryNodesModifierSkipsIncompatibleNodeGroup)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::GeometryNodeAssetTraits group_traits = {};
  blender::bNodeTree geometry_group = {};
  InitGeometryModifierGroup(geometry_group, group_traits, "ToolOnlyGeometry");
  group_traits.flag = blender::GEO_NODE_ASSET_TOOL;

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueString name_default = {};
  blender::bNodeSocketValueString modifier_default = {};
  blender::bNodeSocketValueInt index_default = {};
  blender::bNodeSocketValueInt id_default = {};
  blender::bNode assign_node = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket object = {};
  blender::bNodeSocket name = {};
  blender::bNodeSocket modifier = {};
  blender::bNodeSocket index = {};
  blender::bNodeSocket modifier_id = {};
  InitNode(
      assign_node, "LogicNativeAssignGeometryNodesModifier", "Assign Geometry Nodes Modifier", 2);
  assign_node.id = &geometry_group.id;
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(name, "Modifier Name", blender::SOCK_STRING, &name_default);
  InitSocket(modifier, "Modifier", blender::SOCK_STRING, &modifier_default);
  InitSocket(index, "Index", blender::SOCK_INT, &index_default);
  InitSocket(modifier_id, "Modifier ID", blender::SOCK_INT, &id_default);
  Append(assign_node.inputs, flow);
  Append(assign_node.inputs, object);
  Append(assign_node.inputs, name);
  Append(assign_node.inputs, modifier);
  Append(assign_node.inputs, index);
  Append(assign_node.inputs, modifier_id);
  Append(tree.nodes, assign_node);

  blender::bNodeLink flow_link = {};
  InitLink(flow_link, on_update, on_update_pulse, assign_node, flow);
  Append(tree.links, flow_link);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  EXPECT_TRUE(HasWarningContaining(program->GetCompileReport(), "not modifier-compatible"));
  EXPECT_EQ(FindInstruction(program->GetInstructions(LN_Event::OnUpdate),
                            LN_OpCode::AssignGeometryNodesModifier),
            nullptr);
}

TEST(LN_TreeCompiler, GetMaterialParameterCompilesPerObjectOverrideRead)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueString node_name_default = {};
  SetCString(node_name_default.value, "Principled BSDF");
  blender::bNodeSocketValueString socket_name_default = {};
  SetCString(socket_name_default.value, "Base Color");
  blender::bNodeSocketValueInt slot_default = {};
  slot_default.value = 1;

  blender::bNode get_parameter = {};
  blender::bNodeSocket object = {};
  blender::bNodeSocket slot_socket = {};
  blender::bNodeSocket node_name = {};
  blender::bNodeSocket socket_name = {};
  blender::bNodeSocket value_output = {};
  InitNode(get_parameter, "LogicNativeGetMaterialParameter", "Get Material Parameter", 2);
  get_parameter.custom1 = 4;
  get_parameter.custom2 = 1;
  InitSocket(object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(slot_socket, "Slot", blender::SOCK_INT, &slot_default);
  InitSocket(node_name, "Node Name", blender::SOCK_STRING, &node_name_default);
  InitSocket(socket_name, "Socket", blender::SOCK_STRING, &socket_name_default);
  InitSocket(value_output, "Value", blender::SOCK_CUSTOM, nullptr);
  Append(get_parameter.inputs, object);
  Append(get_parameter.inputs, slot_socket);
  Append(get_parameter.inputs, node_name);
  Append(get_parameter.inputs, socket_name);
  Append(get_parameter.outputs, value_output);
  Append(tree.nodes, get_parameter);

  blender::bNode print_node = {};
  blender::bNodeSocket print_flow = {};
  blender::bNodeSocket print_message = {};
  InitNode(print_node, "LogicNativePrint", "Print", 3);
  InitSocket(print_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(print_message, "Message", blender::SOCK_CUSTOM, nullptr);
  Append(print_node.inputs, print_flow);
  Append(print_node.inputs, print_message);
  Append(tree.nodes, print_node);

  blender::bNodeLink flow_link = {};
  blender::bNodeLink value_link = {};
  InitLink(flow_link, on_update, on_update_pulse, print_node, print_flow);
  InitLink(value_link, get_parameter, value_output, print_node, print_message);
  Append(tree.links, flow_link);
  Append(tree.links, value_link);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());

  const LN_Instruction *print_instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnUpdate), LN_OpCode::Print);
  ASSERT_NE(print_instruction, nullptr);
  const LN_ValueExpression *value_expr = ValueExpressionAt(*program,
                                                           print_instruction->value_expr_index);
  ASSERT_NE(value_expr, nullptr);
  EXPECT_EQ(value_expr->kind, LN_ValueExpressionKind::MaterialNodeValue);
  EXPECT_EQ(value_expr->property_ref_index, 2u);
  EXPECT_EQ(value_expr->input0, LN_INVALID_INDEX);

  const LN_IntExpression *slot_expr = IntExpressionAt(*program, value_expr->input1);
  ASSERT_NE(slot_expr, nullptr);
  EXPECT_EQ(slot_expr->kind, LN_IntExpressionKind::Constant);
  EXPECT_EQ(slot_expr->int_value, 1);

  const LN_StringExpression *compiled_node_name = StringExpressionAt(
      *program, value_expr->string_expr_index);
  ASSERT_NE(compiled_node_name, nullptr);
  EXPECT_EQ(compiled_node_name->kind, LN_StringExpressionKind::Constant);
  EXPECT_EQ(compiled_node_name->string_value, "Principled BSDF");

  const LN_StringExpression *compiled_socket_name = StringExpressionAt(*program,
                                                                       value_expr->input2);
  ASSERT_NE(compiled_socket_name, nullptr);
  EXPECT_EQ(compiled_socket_name->kind, LN_StringExpressionKind::Constant);
  EXPECT_EQ(compiled_socket_name->string_value, "Base Color");
}

TEST(LN_TreeCompiler, AssignMaterialToSlotInputCompilesAlongsideTorqueBranch)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::Material red_material = {};
  InitMaterial(red_material, "Red");

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnFixedUpdate", "On Fixed Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueBoolean condition_default = {};
  condition_default.value = 1;
  blender::bNode branch = {};
  blender::bNodeSocket branch_flow = {};
  blender::bNodeSocket condition = {};
  blender::bNodeSocket true_output = {};
  blender::bNodeSocket false_output = {};
  InitNode(branch, "LogicNativeBranch", "Branch", 2);
  InitSocket(branch_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(condition, "Condition", blender::SOCK_BOOLEAN, &condition_default);
  InitSocket(true_output, "True", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(false_output, "False", blender::SOCK_BOOLEAN, nullptr);
  Append(branch.inputs, branch_flow);
  Append(branch.inputs, condition);
  Append(branch.outputs, true_output);
  Append(branch.outputs, false_output);
  Append(tree.nodes, branch);

  blender::bNodeSocketValueVector torque_default = {};
  torque_default.value[2] = 7.0f;
  blender::bNodeSocketValueBoolean local_default = {};
  local_default.value = 1;
  blender::bNode apply_torque = {};
  blender::bNodeSocket torque_flow = {};
  blender::bNodeSocket torque_object = {};
  blender::bNodeSocket torque = {};
  blender::bNodeSocket local = {};
  InitNode(apply_torque, "LogicNativeApplyTorque", "Apply Torque", 3);
  InitSocket(torque_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(torque_object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(torque, "Torque", blender::SOCK_VECTOR, &torque_default);
  InitSocket(local, "Local", blender::SOCK_BOOLEAN, &local_default);
  Append(apply_torque.inputs, torque_flow);
  Append(apply_torque.inputs, torque_object);
  Append(apply_torque.inputs, torque);
  Append(apply_torque.inputs, local);
  Append(tree.nodes, apply_torque);

  blender::bNodeSocketValueInt slot_default = {};
  slot_default.value = 1;
  blender::bNode set_material = {};
  blender::bNodeSocket material_flow = {};
  blender::bNodeSocket material_object = {};
  blender::bNodeSocketValueMaterial material_default = {};
  material_default.value = &red_material;
  blender::bNodeSocket material = {};
  blender::bNodeSocket slot = {};
  InitNode(set_material, "LogicNativeSetMaterialSlot", "Assign Material To Slot", 4);
  InitSocket(material_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(material_object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(material, "Material", blender::SOCK_MATERIAL, &material_default);
  InitSocket(slot, "Slot", blender::SOCK_INT, &slot_default);
  Append(set_material.inputs, material_flow);
  Append(set_material.inputs, material_object);
  Append(set_material.inputs, material);
  Append(set_material.inputs, slot);
  Append(tree.nodes, set_material);

  blender::bNodeLink event_to_branch = {};
  blender::bNodeLink branch_to_torque = {};
  blender::bNodeLink branch_to_material = {};
  InitLink(event_to_branch, on_update, on_update_pulse, branch, branch_flow);
  InitLink(branch_to_torque, branch, true_output, apply_torque, torque_flow);
  InitLink(branch_to_material, branch, true_output, set_material, material_flow);
  Append(tree.links, event_to_branch);
  Append(tree.links, branch_to_torque);
  Append(tree.links, branch_to_material);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());

  const std::vector<LN_Instruction> &instructions = program->GetInstructions(
      LN_Event::OnFixedUpdate);
  const int route_index = FindInstructionIndex(instructions, LN_OpCode::BranchRoute);
  const int torque_index = FindInstructionIndex(instructions, LN_OpCode::ApplyPhysicsVector);
  const int material_index = FindInstructionIndex(instructions, LN_OpCode::SetMaterialSlot);
  ASSERT_GE(route_index, 0);
  ASSERT_GE(torque_index, 0);
  ASSERT_GE(material_index, 0);

  const LN_Instruction &torque_instruction = instructions[size_t(torque_index)];
  const LN_BoolExpression *torque_guard = BoolExpressionAt(
      *program, torque_instruction.bool_guard_expr_index);
  ASSERT_NE(torque_guard, nullptr);
  EXPECT_EQ(torque_guard->kind, LN_BoolExpressionKind::InstructionExecuted);
  EXPECT_EQ(torque_guard->input0, uint32_t(route_index));

  const LN_Instruction &material_instruction = instructions[size_t(material_index)];
  const LN_BoolExpression *material_guard = BoolExpressionAt(
      *program, material_instruction.bool_guard_expr_index);
  ASSERT_NE(material_guard, nullptr);
  EXPECT_EQ(material_guard->kind, LN_BoolExpressionKind::InstructionExecuted);
  EXPECT_EQ(material_guard->input0, uint32_t(route_index));
  const LN_ValueExpression *material_expr = ValueExpressionAt(
      *program, material_instruction.secondary_value_expr_index);
  ASSERT_NE(material_expr, nullptr);
  EXPECT_EQ(material_expr->kind, LN_ValueExpressionKind::Constant);
  EXPECT_EQ(material_expr->value.type, LN_ValueType::DatablockRef);
  EXPECT_TRUE(material_expr->value.exists);
  EXPECT_EQ(material_expr->value.reference_name, "Red");
}

TEST(LN_TreeCompiler, EmitsSetLinearVelocityFromFixedUpdate)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnFixedUpdate", "On Fixed Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueVector velocity_default = {};
  velocity_default.value[0] = 0.0f;
  velocity_default.value[1] = 6.0f;
  velocity_default.value[2] = 1.5f;
  blender::bNode set_velocity = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket velocity = {};
  SetObjectAttributeAuxSockets set_attribute_aux = {};
  InitSetObjectAttributeVectorNode(
      set_velocity, set_attribute_aux, flow, velocity, "Set Velocity", 2, 2, &velocity_default);
  Append(tree.nodes, set_velocity);

  blender::bNodeLink flow_link = {};
  InitLink(flow_link, on_update, on_update_pulse, set_velocity, flow);
  Append(tree.links, flow_link);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors());
  EXPECT_TRUE(HasVectorInstruction(program->GetInstructions(LN_Event::OnFixedUpdate),
                                   LN_OpCode::SetVelocityVector,
                                   MT_Vector3(0.0f, 6.0f, 1.5f)));
}

TEST(LN_TreeCompiler, EmitsSetLocalScaleFromSetObjectAttribute)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnFixedUpdate", "On Fixed Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueVector scale_default = {};
  scale_default.value[0] = 1.5f;
  scale_default.value[1] = 2.0f;
  scale_default.value[2] = 0.5f;
  blender::bNode set_scale = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket scale = {};
  SetObjectAttributeAuxSockets set_attribute_aux = {};
  InitSetObjectAttributeVectorNode(
      set_scale, set_attribute_aux, flow, scale, "Set Local Scale", 2, 13, &scale_default);
  Append(tree.nodes, set_scale);

  blender::bNodeLink flow_link = {};
  InitLink(flow_link, on_update, on_update_pulse, set_scale, flow);
  Append(tree.links, flow_link);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  const LN_Instruction *instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::SetTransformVector);
  ASSERT_NE(instruction, nullptr);
  EXPECT_EQ(instruction->vector_operation_mode, uint8_t(LN_VectorOperationMode::Local));
  EXPECT_EQ(instruction->vector_operation_channel, uint8_t(LN_VectorOperationChannel::Scale));
  EXPECT_LT((instruction->vector_value - MT_Vector3(1.5f, 2.0f, 0.5f)).length(), 0.0001f);
}

TEST(LN_TreeCompiler, SetObjectAttributeColorWritesFullRgbaWithoutSnapshotRead)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueVector unused_value_default = {};
  blender::bNodeSocketValueRGBA color_default = {};
  color_default.value[0] = 0.2f;
  color_default.value[1] = 0.4f;
  color_default.value[2] = 0.6f;
  color_default.value[3] = 0.8f;
  blender::bNode set_color = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket value = {};
  SetObjectAttributeAuxSockets set_attribute_aux = {};
  InitSetObjectAttributeVectorNode(
      set_color, set_attribute_aux, flow, value, "Set Color", 2, 11, &unused_value_default);
  set_attribute_aux.color.default_value = &color_default;
  Append(tree.nodes, set_color);

  blender::bNodeLink flow_link = {};
  InitLink(flow_link, on_update, on_update_pulse, set_color, flow);
  Append(tree.links, flow_link);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  const LN_Instruction *instruction = FindInstruction(program->GetInstructions(LN_Event::OnUpdate),
                                                      LN_OpCode::SetObjectColor);
  ASSERT_NE(instruction, nullptr);
  const LN_ColorExpression *expression = ColorExpressionAt(*program,
                                                           instruction->color_expr_index);
  ASSERT_NE(expression, nullptr);
  EXPECT_EQ(expression->kind, LN_ColorExpressionKind::Constant);
  EXPECT_LT((expression->color_value - MT_Vector4(0.2f, 0.4f, 0.6f, 0.8f)).length(), 0.0001f);
  EXPECT_LT((instruction->color_value - MT_Vector4(0.2f, 0.4f, 0.6f, 0.8f)).length(), 0.0001f);
}

TEST(LN_TreeCompiler, SetObjectAttributeColorPartialRgbMaskStillWritesInputAlpha)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueVector unused_value_default = {};
  blender::bNodeSocketValueRGBA color_default = {};
  color_default.value[0] = 0.2f;
  color_default.value[1] = 0.4f;
  color_default.value[2] = 0.6f;
  color_default.value[3] = 0.8f;
  blender::bNode set_color = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket value = {};
  SetObjectAttributeAuxSockets set_attribute_aux = {};
  InitSetObjectAttributeVectorNode(
      set_color, set_attribute_aux, flow, value, "Set Masked Color", 2, 11, &unused_value_default);
  set_color.custom2 = 9; /* Initialized mask with only the first RGB component enabled. */
  set_attribute_aux.xyz_default.value[0] = 1.0f;
  set_attribute_aux.xyz_default.value[1] = 0.0f;
  set_attribute_aux.xyz_default.value[2] = 0.0f;
  set_attribute_aux.color.default_value = &color_default;
  Append(tree.nodes, set_color);

  blender::bNodeLink flow_link = {};
  InitLink(flow_link, on_update, on_update_pulse, set_color, flow);
  Append(tree.links, flow_link);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  const LN_Instruction *instruction = FindInstruction(program->GetInstructions(LN_Event::OnUpdate),
                                                      LN_OpCode::SetObjectColor);
  ASSERT_NE(instruction, nullptr);
  const LN_ColorExpression *expression = ColorExpressionAt(*program,
                                                           instruction->color_expr_index);
  ASSERT_NE(expression, nullptr);
  EXPECT_EQ(expression->kind, LN_ColorExpressionKind::Combine);
  const LN_FloatExpression *alpha = FloatExpressionAt(*program, expression->input3);
  ASSERT_NE(alpha, nullptr);
  EXPECT_EQ(alpha->kind, LN_FloatExpressionKind::Constant);
  EXPECT_NEAR(alpha->float_value, 0.8f, 0.0001f);
}

TEST(LN_TreeCompiler, AddRigidBodyConstraintsCompilesFrameAndBodyObjects)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::Object frame_object = {};
  blender::Object first_object = {};
  blender::Object second_object = {};
  SetCString(frame_object.id.name, "OBConstraintFrame");
  SetCString(first_object.id.name, "OBFirstBody");
  SetCString(second_object.id.name, "OBSecondBody");

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnFixedUpdate", "On Fixed Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueObject frame_default = {};
  blender::bNodeSocketValueObject first_default = {};
  blender::bNodeSocketValueObject second_default = {};
  frame_default.value = &frame_object;
  first_default.value = &first_object;
  second_default.value = &second_object;
  blender::bNodeSocketValueString name_default = {};
  SetCString(name_default.value, "runtime_constraint");
  blender::bNodeSocketValueBoolean bool_default = {};
  blender::bNodeSocketValueFloat float_default = {};
  blender::bNodeSocketValueInt int_default = {};
  blender::bNodeSocketValueInt position_int_default = {};
  blender::bNodeSocketValueVector vector_default = {};
  int_default.value = 10;
  position_int_default.value = 2;

  blender::bNode add_constraint = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket first = {};
  blender::bNodeSocket second = {};
  blender::bNodeSocket frame = {};
  blender::bNodeSocket name = {};
  InitNode(add_constraint, "LogicNativeAddPhysicsConstraint", "Add Constraint", 2);
  add_constraint.custom1 = blender::RBC_TYPE_HINGE;
  add_constraint.custom2 = blender::RBC_SPRING_TYPE1;
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocketWithIdentifier(first, "Object", "First", blender::SOCK_OBJECT, &first_default);
  InitSocketWithIdentifier(second, "Target", "Second", blender::SOCK_OBJECT, &second_default);
  InitSocket(frame, "Constraint Object", blender::SOCK_OBJECT, &frame_default);
  InitSocket(name, "Name", blender::SOCK_STRING, &name_default);
  Append(add_constraint.inputs, flow);
  Append(add_constraint.inputs, first);
  Append(add_constraint.inputs, second);
  Append(add_constraint.inputs, frame);
  Append(add_constraint.inputs, name);

  std::vector<std::unique_ptr<blender::bNodeSocket>> extra_sockets;
  auto append_input =
      [&](const char *socket_name, blender::eNodeSocketDatatype socket_type, void *default_value) {
        extra_sockets.push_back(std::make_unique<blender::bNodeSocket>());
        InitSocket(*extra_sockets.back(), socket_name, socket_type, default_value);
        Append(add_constraint.inputs, *extra_sockets.back());
      };
  append_input("Use World Space", blender::SOCK_BOOLEAN, &bool_default);
  append_input("Pivot", blender::SOCK_VECTOR, &vector_default);
  append_input("Rotation", blender::SOCK_VECTOR, &vector_default);
  append_input("Enabled", blender::SOCK_BOOLEAN, &bool_default);
  append_input("Disable Collisions", blender::SOCK_BOOLEAN, &bool_default);
  append_input("Breakable", blender::SOCK_BOOLEAN, &bool_default);
  append_input("Breaking Threshold", blender::SOCK_FLOAT, &float_default);
  append_input("Override Iterations", blender::SOCK_BOOLEAN, &bool_default);
  append_input("Velocity Solver Iterations", blender::SOCK_INT, &int_default);
  append_input("Position Solver Iterations", blender::SOCK_INT, &position_int_default);
  append_input("Use Linear Limit X", blender::SOCK_BOOLEAN, &bool_default);
  append_input("Use Linear Limit Y", blender::SOCK_BOOLEAN, &bool_default);
  append_input("Use Linear Limit Z", blender::SOCK_BOOLEAN, &bool_default);
  append_input("Linear Lower", blender::SOCK_VECTOR, &vector_default);
  append_input("Linear Upper", blender::SOCK_VECTOR, &vector_default);
  append_input("Use Angular Limit X", blender::SOCK_BOOLEAN, &bool_default);
  append_input("Use Angular Limit Y", blender::SOCK_BOOLEAN, &bool_default);
  append_input("Use Angular Limit Z", blender::SOCK_BOOLEAN, &bool_default);
  append_input("Angular Lower", blender::SOCK_VECTOR, &vector_default);
  append_input("Angular Upper", blender::SOCK_VECTOR, &vector_default);
  append_input("Use Spring X", blender::SOCK_BOOLEAN, &bool_default);
  append_input("Use Spring Y", blender::SOCK_BOOLEAN, &bool_default);
  append_input("Use Spring Z", blender::SOCK_BOOLEAN, &bool_default);
  append_input("Spring Stiffness", blender::SOCK_VECTOR, &vector_default);
  append_input("Spring Damping", blender::SOCK_VECTOR, &vector_default);
  append_input("Use Angular Spring X", blender::SOCK_BOOLEAN, &bool_default);
  append_input("Use Angular Spring Y", blender::SOCK_BOOLEAN, &bool_default);
  append_input("Use Angular Spring Z", blender::SOCK_BOOLEAN, &bool_default);
  append_input("Angular Spring Stiffness", blender::SOCK_VECTOR, &vector_default);
  append_input("Angular Spring Damping", blender::SOCK_VECTOR, &vector_default);
  append_input("Use Linear Motor", blender::SOCK_BOOLEAN, &bool_default);
  append_input("Linear Motor Target Velocity", blender::SOCK_FLOAT, &float_default);
  append_input("Linear Motor Max Impulse", blender::SOCK_FLOAT, &float_default);
  append_input("Use Angular Motor", blender::SOCK_BOOLEAN, &bool_default);
  append_input("Angular Motor Target Velocity", blender::SOCK_FLOAT, &float_default);
  append_input("Angular Motor Max Impulse", blender::SOCK_FLOAT, &float_default);
  Append(tree.nodes, add_constraint);

  blender::bNodeLink flow_link = {};
  InitLink(flow_link, on_update, on_update_pulse, add_constraint, flow);
  Append(tree.links, flow_link);

  const std::pair<int, PHY_RigidBodyConstraintType> expected_types[] = {
      {blender::RBC_TYPE_POINT, PHY_RigidBodyConstraintType::Point},
      {blender::RBC_TYPE_HINGE, PHY_RigidBodyConstraintType::Hinge},
      {blender::RBC_TYPE_SLIDER, PHY_RigidBodyConstraintType::Slider},
      {blender::RBC_TYPE_6DOF, PHY_RigidBodyConstraintType::Generic},
      {blender::RBC_TYPE_6DOF_SPRING, PHY_RigidBodyConstraintType::GenericSpring},
      {blender::RBC_TYPE_FIXED, PHY_RigidBodyConstraintType::Fixed},
      {blender::RBC_TYPE_PISTON, PHY_RigidBodyConstraintType::Piston},
      {blender::RBC_TYPE_MOTOR, PHY_RigidBodyConstraintType::Motor},
  };

  for (const auto &[node_type, expected_type] : expected_types) {
    add_constraint.custom1 = node_type;
    const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
    EXPECT_FALSE(program->GetCompileReport().HasErrors())
        << FormatCompileReport(program->GetCompileReport());
    ASSERT_TRUE(program->ValidateInstructionPayloads());

    const LN_Instruction *instruction = FindInstruction(
        program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::AddPhysicsConstraint);
    ASSERT_NE(instruction, nullptr);
    ASSERT_EQ(program->GetRigidBodyConstraintCommandPayloads().size(), 1u);
    const LN_RigidBodyConstraintCommandPayload &payload =
        program->GetRigidBodyConstraintCommandPayloads()[instruction->command_payload_index];
    EXPECT_EQ(payload.type, expected_type);
    EXPECT_EQ(payload.spring_type, PHY_RigidBodyConstraintSpringType::Spring2);
    EXPECT_NE(payload.velocity_solver_iterations_int_expr_index, LN_INVALID_INDEX);
    EXPECT_NE(payload.position_solver_iterations_int_expr_index, LN_INVALID_INDEX);
    for (const uint32_t bool_expr_index : payload.bool_expr_indices) {
      EXPECT_NE(bool_expr_index, LN_INVALID_INDEX);
    }
    for (const uint32_t vector_expr_index : payload.vector_expr_indices) {
      EXPECT_NE(vector_expr_index, LN_INVALID_INDEX);
    }
    for (const uint32_t float_expr_index : payload.float_expr_indices) {
      EXPECT_NE(float_expr_index, LN_INVALID_INDEX);
    }

    const LN_ValueExpression *frame_expr = ValueExpressionAt(
        *program, payload.constraint_object_value_expr_index);
    const LN_ValueExpression *first_expr = ValueExpressionAt(*program,
                                                             payload.object_value_expr_index);
    const LN_ValueExpression *second_expr = ValueExpressionAt(*program,
                                                              payload.target_value_expr_index);
    ASSERT_NE(frame_expr, nullptr);
    ASSERT_NE(first_expr, nullptr);
    ASSERT_NE(second_expr, nullptr);
    EXPECT_EQ(frame_expr->value.reference_name, "ConstraintFrame");
    EXPECT_EQ(first_expr->value.reference_name, "FirstBody");
    EXPECT_EQ(second_expr->value.reference_name, "SecondBody");
  }
}

TEST(LN_TreeCompiler, RigidBodyConstraintManagementCompilesLookupAndRemoveAll)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::Object first_object = {};
  SetCString(first_object.id.name, "OBFirstBody");
  blender::bNodeSocketValueObject first_default = {};
  first_default.value = &first_object;
  blender::bNodeSocketValueString name_default = {};
  SetCString(name_default.value, "runtime_constraint");
  blender::bNodeSocketValueBoolean remove_all_default = {};
  remove_all_default.value = true;

  blender::bNode get_constraints = {};
  blender::bNodeSocket get_first = {};
  blender::bNodeSocket get_name = {};
  blender::bNodeSocket found = {};
  blender::bNodeSocket constraint = {};
  blender::bNodeSocket constraints = {};
  InitNode(get_constraints, "LogicNativeGetRigidBodyConstraints", "Get Rigid Body Constraints", 1);
  get_constraints.custom1 = int(LN_RigidBodyConstraintMatchMode::Contains);
  InitSocketWithIdentifier(get_first, "Object", "First", blender::SOCK_OBJECT, &first_default);
  InitSocket(get_name, "Name", blender::SOCK_STRING, &name_default);
  InitSocket(found, "Found", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(constraint, "Constraint", blender::SOCK_STRING, nullptr);
  InitSocket(constraints, "Constraints", blender::SOCK_CUSTOM, nullptr, "NodeSocketLogicList");
  Append(get_constraints.inputs, get_first);
  Append(get_constraints.inputs, get_name);
  Append(get_constraints.outputs, found);
  Append(get_constraints.outputs, constraint);
  Append(get_constraints.outputs, constraints);
  Append(tree.nodes, get_constraints);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnFixedUpdate", "On Fixed Update", 2);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNode remove_constraints = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket remove_first = {};
  blender::bNodeSocket remove_all = {};
  blender::bNodeSocket remove_name = {};
  blender::bNodeSocket done = {};
  InitNode(remove_constraints,
           "LogicNativeRemovePhysicsConstraint",
           "Remove Rigid Body Constraints",
           3);
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocketWithIdentifier(remove_first, "Object", "First", blender::SOCK_OBJECT, &first_default);
  InitSocket(remove_all, "Remove All", blender::SOCK_BOOLEAN, &remove_all_default);
  InitSocket(remove_name, "Name", blender::SOCK_STRING, &name_default);
  InitSocket(done, "Done", blender::SOCK_BOOLEAN, nullptr);
  Append(remove_constraints.inputs, flow);
  Append(remove_constraints.inputs, remove_first);
  Append(remove_constraints.inputs, remove_all);
  Append(remove_constraints.inputs, remove_name);
  Append(remove_constraints.outputs, done);
  Append(tree.nodes, remove_constraints);

  blender::bNode print_matches = {};
  blender::bNodeSocket print_flow = {};
  blender::bNodeSocket print_message = {};
  InitNode(print_matches, "LogicNativePrint", "Print Matches", 4);
  InitSocket(print_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(print_message, "Message", blender::SOCK_CUSTOM, nullptr);
  Append(print_matches.inputs, print_flow);
  Append(print_matches.inputs, print_message);
  Append(tree.nodes, print_matches);

  blender::bNodeLink flow_link = {};
  blender::bNodeLink print_flow_link = {};
  blender::bNodeLink found_to_remove_all = {};
  blender::bNodeLink constraint_to_name = {};
  blender::bNodeLink constraints_to_print = {};
  InitLink(flow_link, on_update, on_update_pulse, remove_constraints, flow);
  InitLink(print_flow_link, on_update, on_update_pulse, print_matches, print_flow);
  InitLink(found_to_remove_all, get_constraints, found, remove_constraints, remove_all);
  InitLink(constraint_to_name, get_constraints, constraint, remove_constraints, remove_name);
  InitLink(constraints_to_print, get_constraints, constraints, print_matches, print_message);
  Append(tree.links, flow_link);
  Append(tree.links, print_flow_link);
  Append(tree.links, found_to_remove_all);
  Append(tree.links, constraint_to_name);
  Append(tree.links, constraints_to_print);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  EXPECT_NE(std::find_if(program->GetBoolExpressions().begin(),
                         program->GetBoolExpressions().end(),
                         [](const LN_BoolExpression &expression) {
                           return expression.kind ==
                                  LN_BoolExpressionKind::RigidBodyConstraintFound;
                         }),
            program->GetBoolExpressions().end());
  EXPECT_NE(std::find_if(program->GetStringExpressions().begin(),
                         program->GetStringExpressions().end(),
                         [](const LN_StringExpression &expression) {
                           return expression.kind ==
                                  LN_StringExpressionKind::RigidBodyConstraintName;
                         }),
            program->GetStringExpressions().end());
  EXPECT_NE(std::find_if(program->GetValueExpressions().begin(),
                         program->GetValueExpressions().end(),
                         [](const LN_ValueExpression &expression) {
                           return expression.kind ==
                                  LN_ValueExpressionKind::RigidBodyConstraintNames;
                         }),
            program->GetValueExpressions().end());

  const LN_Instruction *remove_instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::RemovePhysicsConstraint);
  ASSERT_NE(remove_instruction, nullptr);
  const LN_BoolExpression *remove_all_expression = BoolExpressionAt(
      *program, remove_instruction->bool_expr_index);
  ASSERT_NE(remove_all_expression, nullptr);
  EXPECT_EQ(remove_all_expression->kind, LN_BoolExpressionKind::RigidBodyConstraintFound);
  EXPECT_EQ(remove_all_expression->rigid_body_constraint_match_mode,
            LN_RigidBodyConstraintMatchMode::Contains);
  const LN_StringExpression *remove_name_expression = StringExpressionAt(
      *program, remove_instruction->string_expr_index);
  ASSERT_NE(remove_name_expression, nullptr);
  EXPECT_EQ(remove_name_expression->kind, LN_StringExpressionKind::RigidBodyConstraintName);
  EXPECT_EQ(remove_name_expression->rigid_body_constraint_match_mode,
            LN_RigidBodyConstraintMatchMode::Contains);

  const auto matches_expression = std::find_if(
      program->GetValueExpressions().begin(),
      program->GetValueExpressions().end(),
      [](const LN_ValueExpression &expression) {
        return expression.kind == LN_ValueExpressionKind::RigidBodyConstraintNames;
      });
  ASSERT_NE(matches_expression, program->GetValueExpressions().end());
  EXPECT_EQ(matches_expression->rigid_body_constraint_match_mode,
            LN_RigidBodyConstraintMatchMode::Contains);
  EXPECT_NE(matches_expression->string_expr_index, LN_INVALID_INDEX);
}

TEST(LN_TreeCompiler, RigidBodyConstraintAllModeCompilesOnlyConnectedOutput)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode get_constraints = {};
  blender::bNodeSocket get_first = {};
  blender::bNodeSocket get_name = {};
  blender::bNodeSocket found = {};
  blender::bNodeSocket constraint = {};
  blender::bNodeSocket constraints = {};
  InitNode(get_constraints, "LogicNativeGetRigidBodyConstraints", "Get Rigid Body Constraints", 1);
  get_constraints.custom1 = int(LN_RigidBodyConstraintMatchMode::All);
  InitSocketWithIdentifier(get_first, "Object", "First", blender::SOCK_OBJECT, nullptr);
  InitSocket(get_name, "Name", blender::SOCK_STRING, nullptr);
  InitSocket(found, "Found", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(constraint, "Constraint", blender::SOCK_STRING, nullptr);
  InitSocket(constraints, "Constraints", blender::SOCK_CUSTOM, nullptr, "NodeSocketLogicList");
  Append(get_constraints.inputs, get_first);
  Append(get_constraints.inputs, get_name);
  Append(get_constraints.outputs, found);
  Append(get_constraints.outputs, constraint);
  Append(get_constraints.outputs, constraints);
  Append(tree.nodes, get_constraints);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 2);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNode print_found = {};
  blender::bNodeSocket print_flow = {};
  blender::bNodeSocket print_message = {};
  InitNode(print_found, "LogicNativePrint", "Print Found", 3);
  InitSocket(print_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(print_message, "Message", blender::SOCK_CUSTOM, nullptr);
  Append(print_found.inputs, print_flow);
  Append(print_found.inputs, print_message);
  Append(tree.nodes, print_found);

  blender::bNodeLink flow_link = {};
  blender::bNodeLink value_link = {};
  InitLink(flow_link, on_update, on_update_pulse, print_found, print_flow);
  InitLink(value_link, get_constraints, found, print_found, print_message);
  Append(tree.links, flow_link);
  Append(tree.links, value_link);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  const auto found_expression = std::find_if(
      program->GetBoolExpressions().begin(),
      program->GetBoolExpressions().end(),
      [](const LN_BoolExpression &expression) {
        return expression.kind == LN_BoolExpressionKind::RigidBodyConstraintFound;
      });
  ASSERT_NE(found_expression, program->GetBoolExpressions().end());
  EXPECT_EQ(found_expression->rigid_body_constraint_match_mode,
            LN_RigidBodyConstraintMatchMode::All);
  EXPECT_EQ(found_expression->property_ref_index, LN_INVALID_INDEX);
  EXPECT_EQ(std::count_if(program->GetStringExpressions().begin(),
                          program->GetStringExpressions().end(),
                          [](const LN_StringExpression &expression) {
                            return expression.kind ==
                                   LN_StringExpressionKind::RigidBodyConstraintName;
                          }),
            0);
  EXPECT_EQ(std::count_if(program->GetValueExpressions().begin(),
                          program->GetValueExpressions().end(),
                          [](const LN_ValueExpression &expression) {
                            return expression.kind ==
                                   LN_ValueExpressionKind::RigidBodyConstraintNames;
                          }),
            0);
}

TEST(LN_TreeCompiler, WarnsAndSkipsSetLinearVelocityFromValueBoolFlow)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNodeSocketValueBoolean value_default = {};
  value_default.value = 1;
  blender::bNode value = {};
  blender::bNodeSocket value_output = {};
  InitNode(value, "LogicNativeValueBool", "Bool", 1);
  InitSocket(value_output, "Bool", blender::SOCK_BOOLEAN, &value_default);
  Append(value.outputs, value_output);
  Append(tree.nodes, value);

  blender::bNodeSocketValueVector velocity_default = {};
  velocity_default.value[1] = 6.0f;
  blender::bNode set_velocity = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket velocity = {};
  SetObjectAttributeAuxSockets set_attribute_aux = {};
  InitSetObjectAttributeVectorNode(
      set_velocity, set_attribute_aux, flow, velocity, "Set Velocity", 2, 2, &velocity_default);
  Append(tree.nodes, set_velocity);

  blender::bNodeLink value_to_flow = {};
  InitLink(value_to_flow, value, value_output, set_velocity, flow);
  Append(tree.links, value_to_flow);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors());
  EXPECT_TRUE(HasWarningContaining(program->GetCompileReport(), "requires an execution output"))
      << FormatCompileReport(program->GetCompileReport());
  const LN_Instruction *instruction = FindInstruction(program->GetInstructions(LN_Event::OnUpdate),
                                                      LN_OpCode::SetVelocityVector);
  EXPECT_EQ(instruction, nullptr);
}

TEST(LN_TreeCompiler, EmitsSetWorldPositionFromSnapshotWorldPosition)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnFixedUpdate", "On Fixed Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNode get_position = {};
  blender::bNodeSocket get_position_output = {};
  GetObjectAttributeAuxSockets get_attribute_aux = {};
  InitGetObjectAttributeVectorNode(
      get_position, get_attribute_aux, get_position_output, "Get Position", 2, 0);
  Append(tree.nodes, get_position);

  blender::bNodeSocketValueVector position_default = {};
  blender::bNode set_position = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket position = {};
  SetObjectAttributeAuxSockets set_attribute_aux = {};
  InitSetObjectAttributeVectorNode(
      set_position, set_attribute_aux, flow, position, "Set Position", 3, 0, &position_default);
  Append(tree.nodes, set_position);

  blender::bNodeLink flow_link = {};
  blender::bNodeLink position_link = {};
  InitLink(flow_link, on_update, on_update_pulse, set_position, flow);
  InitLink(position_link, get_position, get_position_output, set_position, position);
  Append(tree.links, flow_link);
  Append(tree.links, position_link);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors());
  const LN_Instruction *instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::SetTransformVector);
  ASSERT_NE(instruction, nullptr);
  const LN_VectorExpression *expression = InstructionVectorExpression(*program, *instruction);
  ASSERT_NE(expression, nullptr);
  ExpectObjectAttributeVectorExpression(*program, *expression, "worldPosition");
}

TEST(LN_TreeCompiler, EmitsSetLinearVelocityFromSnapshotLinearVelocity)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnFixedUpdate", "On Fixed Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNode get_velocity = {};
  blender::bNodeSocket get_velocity_output = {};
  GetObjectAttributeAuxSockets get_attribute_aux = {};
  InitGetObjectAttributeVectorNode(
      get_velocity, get_attribute_aux, get_velocity_output, "Get Velocity", 2, 9);
  Append(tree.nodes, get_velocity);

  blender::bNodeSocketValueVector velocity_default = {};
  blender::bNode set_velocity = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket velocity = {};
  SetObjectAttributeAuxSockets set_attribute_aux = {};
  InitSetObjectAttributeVectorNode(
      set_velocity, set_attribute_aux, flow, velocity, "Set Velocity", 3, 2, &velocity_default);
  Append(tree.nodes, set_velocity);

  blender::bNodeLink flow_link = {};
  blender::bNodeLink velocity_link = {};
  InitLink(flow_link, on_update, on_update_pulse, set_velocity, flow);
  InitLink(velocity_link, get_velocity, get_velocity_output, set_velocity, velocity);
  Append(tree.links, flow_link);
  Append(tree.links, velocity_link);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors());
  const LN_Instruction *instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::SetVelocityVector);
  ASSERT_NE(instruction, nullptr);
  const LN_VectorExpression *expression = InstructionVectorExpression(*program, *instruction);
  ASSERT_NE(expression, nullptr);
  ExpectObjectAttributeVectorExpression(*program, *expression, "worldLinearVelocity");
}

TEST(LN_TreeCompiler, EmitsVectorMathExpressionForSetWorldPosition)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnFixedUpdate", "On Fixed Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNode get_position = {};
  blender::bNodeSocket get_position_output = {};
  GetObjectAttributeAuxSockets get_attribute_aux = {};
  InitGetObjectAttributeVectorNode(
      get_position, get_attribute_aux, get_position_output, "Get Position", 2, 0);
  Append(tree.nodes, get_position);

  blender::bNodeSocketValueVector offset_default = {};
  offset_default.value[0] = 1.0f;
  offset_default.value[1] = 0.0f;
  offset_default.value[2] = 0.0f;
  blender::bNode offset = {};
  blender::bNodeSocket offset_output = {};
  InitNode(offset, "LogicNativeValueVector", "Offset", 3);
  InitSocket(offset_output, "Vector", blender::SOCK_VECTOR, &offset_default);
  Append(offset.outputs, offset_output);
  Append(tree.nodes, offset);

  blender::bNodeSocketValueVector math_a_default = {};
  blender::bNodeSocketValueVector math_b_default = {};
  blender::bNodeSocketValueFloat scale_default = {};
  scale_default.value = 1.0f;
  blender::bNode vector_math = {};
  blender::bNodeSocket math_a = {};
  blender::bNodeSocket math_b = {};
  blender::bNodeSocket math_scale = {};
  blender::bNodeSocket math_result = {};
  InitNode(vector_math, "LogicNativeVectorMath", "Vector Math", 4);
  vector_math.custom1 = blender::NODE_VECTOR_MATH_ADD;
  InitSocket(math_a, "A", blender::SOCK_VECTOR, &math_a_default);
  InitSocket(math_b, "B", blender::SOCK_VECTOR, &math_b_default);
  InitSocket(math_scale, "Scale", blender::SOCK_FLOAT, &scale_default);
  InitSocket(math_result, "Result", blender::SOCK_VECTOR, nullptr);
  Append(vector_math.inputs, math_a);
  Append(vector_math.inputs, math_b);
  Append(vector_math.inputs, math_scale);
  Append(vector_math.outputs, math_result);
  Append(tree.nodes, vector_math);

  blender::bNodeSocketValueVector position_default = {};
  blender::bNode set_position = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket position = {};
  SetObjectAttributeAuxSockets set_attribute_aux = {};
  InitSetObjectAttributeVectorNode(
      set_position, set_attribute_aux, flow, position, "Set Position", 5, 0, &position_default);
  Append(tree.nodes, set_position);

  blender::bNodeLink flow_link = {};
  blender::bNodeLink position_to_math = {};
  blender::bNodeLink offset_to_math = {};
  blender::bNodeLink math_to_position = {};
  InitLink(flow_link, on_update, on_update_pulse, set_position, flow);
  InitLink(position_to_math, get_position, get_position_output, vector_math, math_a);
  InitLink(offset_to_math, offset, offset_output, vector_math, math_b);
  InitLink(math_to_position, vector_math, math_result, set_position, position);
  Append(tree.links, flow_link);
  Append(tree.links, position_to_math);
  Append(tree.links, offset_to_math);
  Append(tree.links, math_to_position);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors());
  const LN_Instruction *instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::SetTransformVector);
  ASSERT_NE(instruction, nullptr);
  const LN_VectorExpression *expression = InstructionVectorExpression(*program, *instruction);
  ASSERT_NE(expression, nullptr);
  EXPECT_EQ(expression->kind, LN_VectorExpressionKind::Add);

  const LN_VectorExpression *left = VectorExpressionAt(*program, expression->input0);
  const LN_VectorExpression *right = VectorExpressionAt(*program, expression->input1);
  ASSERT_NE(left, nullptr);
  ASSERT_NE(right, nullptr);
  ExpectObjectAttributeVectorExpression(*program, *left, "worldPosition");
  EXPECT_EQ(right->kind, LN_VectorExpressionKind::Constant);
  EXPECT_LT((right->vector_value - MT_Vector3(1.0f, 0.0f, 0.0f)).length(), 0.0001f);
}

TEST(LN_TreeCompiler, RejectsUnsupportedVectorMathOperation)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueVector vector_default = {};
  blender::bNodeSocketValueFloat scale_default = {};
  scale_default.value = 1.0f;
  blender::bNode vector_math = {};
  blender::bNodeSocket input_a = {};
  blender::bNodeSocket input_b = {};
  blender::bNodeSocket scale = {};
  blender::bNodeSocket result = {};
  InitNode(vector_math, "LogicNativeVectorMath", "Vector Math", 2);
  vector_math.custom1 = 999;
  InitSocket(input_a, "A", blender::SOCK_VECTOR, &vector_default);
  InitSocket(input_b, "B", blender::SOCK_VECTOR, &vector_default);
  InitSocket(scale, "Scale", blender::SOCK_FLOAT, &scale_default);
  InitSocket(result, "Result", blender::SOCK_VECTOR, nullptr);
  Append(vector_math.inputs, input_a);
  Append(vector_math.inputs, input_b);
  Append(vector_math.inputs, scale);
  Append(vector_math.outputs, result);
  Append(tree.nodes, vector_math);

  blender::bNodeSocketValueVector position_default = {};
  blender::bNode set_position = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket position = {};
  SetObjectAttributeAuxSockets set_attribute_aux = {};
  InitSetObjectAttributeVectorNode(
      set_position, set_attribute_aux, flow, position, "Set Position", 3, 0, &position_default);
  Append(tree.nodes, set_position);

  blender::bNodeLink math_to_position = {};
  blender::bNodeLink event_to_set_position = {};
  InitLink(math_to_position, vector_math, result, set_position, position);
  InitLink(event_to_set_position, on_update, on_update_pulse, set_position, flow);
  Append(tree.links, math_to_position);
  Append(tree.links, event_to_set_position);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_TRUE(program->GetCompileReport().HasErrors());
}

TEST(LN_TreeCompiler, RejectsUnsupportedMathOperation)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueFloat a_default = {};
  blender::bNodeSocketValueFloat b_default = {};
  blender::bNode math = {};
  blender::bNodeSocket math_a = {};
  blender::bNodeSocket math_b = {};
  blender::bNodeSocket math_result = {};
  InitNode(math, "LogicNativeMath", "Math", 2);
  math.custom1 = 999;
  InitSocket(math_a, "A", blender::SOCK_FLOAT, &a_default);
  InitSocket(math_b, "B", blender::SOCK_FLOAT, &b_default);
  InitSocket(math_result, "Result", blender::SOCK_FLOAT, nullptr);
  Append(math.inputs, math_a);
  Append(math.inputs, math_b);
  Append(math.outputs, math_result);
  Append(tree.nodes, math);

  blender::bNodeSocketValueVector position_default = {};
  blender::bNode set_position = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket position = {};
  SetObjectAttributeAuxSockets set_attribute_aux = {};
  InitSetObjectAttributeVectorNode(
      set_position, set_attribute_aux, flow, position, "Set Position", 3, 0, &position_default);
  Append(tree.nodes, set_position);

  blender::bNodeLink math_to_position = {};
  blender::bNodeLink event_to_set_position = {};
  InitLink(math_to_position, math, math_result, set_position, position);
  InitLink(event_to_set_position, on_update, on_update_pulse, set_position, flow);
  Append(tree.links, math_to_position);
  Append(tree.links, event_to_set_position);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_TRUE(program->GetCompileReport().HasErrors());
}

TEST(LN_TreeCompiler, AcceptsReroutedFlowAndValueLinks)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnFixedUpdate", "On Fixed Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNode flow_reroute = {};
  blender::bNodeSocket flow_input = {};
  blender::bNodeSocket flow_output = {};
  InitNode(flow_reroute, "NodeReroute", "Flow Reroute", 2);
  InitSocket(flow_input, "Input", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(flow_output, "Output", blender::SOCK_BOOLEAN, nullptr);
  Append(flow_reroute.inputs, flow_input);
  Append(flow_reroute.outputs, flow_output);
  Append(tree.nodes, flow_reroute);

  blender::bNodeSocketValueVector position_source_default = {};
  position_source_default.value[0] = 1.0f;
  position_source_default.value[1] = 2.0f;
  position_source_default.value[2] = 3.0f;
  blender::bNode position_source = {};
  blender::bNodeSocket position_source_output = {};
  InitNode(position_source, "LogicNativeValueVector", "Position", 3);
  InitSocket(position_source_output, "Vector", blender::SOCK_VECTOR, &position_source_default);
  Append(position_source.outputs, position_source_output);
  Append(tree.nodes, position_source);

  blender::bNode position_reroute = {};
  blender::bNodeSocket position_input = {};
  blender::bNodeSocket position_output = {};
  InitNode(position_reroute, "NodeReroute", "Position Reroute", 4);
  InitSocket(position_input, "Input", blender::SOCK_VECTOR, nullptr);
  InitSocket(position_output, "Output", blender::SOCK_VECTOR, nullptr);
  Append(position_reroute.inputs, position_input);
  Append(position_reroute.outputs, position_output);
  Append(tree.nodes, position_reroute);

  blender::bNodeSocketValueVector position_default = {};
  blender::bNode set_position = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket position = {};
  SetObjectAttributeAuxSockets set_attribute_aux = {};
  InitSetObjectAttributeVectorNode(
      set_position, set_attribute_aux, flow, position, "Set Position", 5, 0, &position_default);
  Append(tree.nodes, set_position);

  blender::bNodeLink event_to_reroute = {};
  blender::bNodeLink reroute_to_flow = {};
  blender::bNodeLink position_to_reroute = {};
  blender::bNodeLink reroute_to_position = {};
  InitLink(event_to_reroute, on_update, on_update_pulse, flow_reroute, flow_input);
  InitLink(reroute_to_flow, flow_reroute, flow_output, set_position, flow);
  InitLink(position_to_reroute,
           position_source,
           position_source_output,
           position_reroute,
           position_input);
  InitLink(reroute_to_position, position_reroute, position_output, set_position, position);
  Append(tree.links, event_to_reroute);
  Append(tree.links, reroute_to_flow);
  Append(tree.links, position_to_reroute);
  Append(tree.links, reroute_to_position);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors());
  EXPECT_TRUE(HasVectorInstruction(program->GetInstructions(LN_Event::OnFixedUpdate),
                                   LN_OpCode::SetTransformVector,
                                   MT_Vector3(1.0f, 2.0f, 3.0f)));
}

TEST(LN_TreeCompiler, IgnoresFrameNodes)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode frame = {};
  InitNode(frame, "NodeFrame", "Frame", 99);
  Append(tree.nodes, frame);

  blender::bNode on_init = {};
  blender::bNodeSocket on_init_pulse = {};
  InitNode(on_init, "LogicNativeOnInit", "On Init", 1);
  InitSocket(on_init_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_init.outputs, on_init_pulse);
  Append(tree.nodes, on_init);

  blender::bNodeSocketValueVector position_default = {};
  position_default.value[0] = 2.0f;
  position_default.value[1] = 3.0f;
  position_default.value[2] = 4.0f;
  blender::bNode set_position = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket position = {};
  SetObjectAttributeAuxSockets set_attribute_aux = {};
  InitSetObjectAttributeVectorNode(
      set_position, set_attribute_aux, flow, position, "Set Position", 2, 0, &position_default);
  Append(tree.nodes, set_position);

  blender::bNodeLink flow_link = {};
  InitLink(flow_link, on_init, on_init_pulse, set_position, flow);
  Append(tree.links, flow_link);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors());
  EXPECT_TRUE(HasVectorInstruction(program->GetInstructions(LN_Event::OnInit),
                                   LN_OpCode::SetTransformVector,
                                   MT_Vector3(2.0f, 3.0f, 4.0f)));
}

TEST(LN_TreeCompiler, EmitsApplyImpulseFromFixedUpdate)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnFixedUpdate", "On Fixed Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueVector attach_default = {};
  attach_default.value[0] = 0.5f;
  attach_default.value[1] = 0.0f;
  attach_default.value[2] = 0.0f;
  blender::bNodeSocketValueVector impulse_default = {};
  impulse_default.value[0] = 0.0f;
  impulse_default.value[1] = 9.0f;
  impulse_default.value[2] = 0.0f;
  blender::bNode apply_impulse = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket attach = {};
  blender::bNodeSocket impulse = {};
  InitNode(apply_impulse, "LogicNativeApplyImpulse", "Apply Impulse", 2);
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(attach, "Attach", blender::SOCK_VECTOR, &attach_default);
  InitSocket(impulse, "Impulse", blender::SOCK_VECTOR, &impulse_default);
  Append(apply_impulse.inputs, flow);
  Append(apply_impulse.inputs, attach);
  Append(apply_impulse.inputs, impulse);
  Append(tree.nodes, apply_impulse);

  blender::bNodeLink flow_link = {};
  InitLink(flow_link, on_update, on_update_pulse, apply_impulse, flow);
  Append(tree.links, flow_link);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors());
  EXPECT_TRUE(HasApplyImpulseInstruction(program->GetInstructions(LN_Event::OnFixedUpdate),
                                         MT_Vector3(0.5f, 0.0f, 0.0f),
                                         MT_Vector3(0.0f, 9.0f, 0.0f)));
}

TEST(LN_TreeCompiler, WarnsAndSkipsApplyImpulseFromValueBoolFlow)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNodeSocketValueBoolean value_default = {};
  value_default.value = 1;
  blender::bNode value = {};
  blender::bNodeSocket value_output = {};
  InitNode(value, "LogicNativeValueBool", "Bool", 1);
  InitSocket(value_output, "Bool", blender::SOCK_BOOLEAN, &value_default);
  Append(value.outputs, value_output);
  Append(tree.nodes, value);

  blender::bNodeSocketValueVector attach_default = {};
  blender::bNodeSocketValueVector impulse_default = {};
  impulse_default.value[1] = 9.0f;
  blender::bNode apply_impulse = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket attach = {};
  blender::bNodeSocket impulse = {};
  InitNode(apply_impulse, "LogicNativeApplyImpulse", "Apply Impulse", 2);
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(attach, "Attach", blender::SOCK_VECTOR, &attach_default);
  InitSocket(impulse, "Impulse", blender::SOCK_VECTOR, &impulse_default);
  Append(apply_impulse.inputs, flow);
  Append(apply_impulse.inputs, attach);
  Append(apply_impulse.inputs, impulse);
  Append(tree.nodes, apply_impulse);

  blender::bNodeLink value_to_flow = {};
  InitLink(value_to_flow, value, value_output, apply_impulse, flow);
  Append(tree.links, value_to_flow);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors());
  EXPECT_TRUE(HasWarningContaining(program->GetCompileReport(), "requires an execution output"))
      << FormatCompileReport(program->GetCompileReport());
  const LN_Instruction *instruction = FindInstruction(program->GetInstructions(LN_Event::OnUpdate),
                                                      LN_OpCode::ApplyImpulse);
  EXPECT_EQ(instruction, nullptr);
}

TEST(LN_TreeCompiler, WarnsAndSkipsSetWorldPositionFromValueBoolFlow)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNodeSocketValueBoolean value_default = {};
  value_default.value = 1;
  blender::bNode value = {};
  blender::bNodeSocket value_output = {};
  InitNode(value, "LogicNativeValueBool", "Bool", 1);
  InitSocket(value_output, "Bool", blender::SOCK_BOOLEAN, &value_default);
  Append(value.outputs, value_output);
  Append(tree.nodes, value);

  blender::bNodeSocketValueVector position_default = {};
  position_default.value[0] = 2.0f;
  blender::bNode set_position = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket position = {};
  SetObjectAttributeAuxSockets set_attribute_aux = {};
  InitSetObjectAttributeVectorNode(
      set_position, set_attribute_aux, flow, position, "Set Position", 2, 0, &position_default);
  Append(tree.nodes, set_position);

  blender::bNodeLink value_to_flow = {};
  InitLink(value_to_flow, value, value_output, set_position, flow);
  Append(tree.links, value_to_flow);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors());
  EXPECT_TRUE(HasWarningContaining(program->GetCompileReport(), "requires an execution output"))
      << FormatCompileReport(program->GetCompileReport());
  const LN_Instruction *instruction = FindInstruction(program->GetInstructions(LN_Event::OnUpdate),
                                                      LN_OpCode::SetTransformVector);
  EXPECT_EQ(instruction, nullptr);
}

TEST(LN_TreeCompiler, KeyboardKeyCompilesInputStatusCodeAndStatus)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNodeSocketValueString key_default = {};
  SetCString(key_default.value, "bge.events.space");
  blender::bNode keyboard = {};
  blender::bNodeSocket key_input = {};
  blender::bNodeSocket keyboard_out = {};
  InitNode(keyboard, "LogicNativeKeyboardKey", "Keyboard Key", 1);
  InitSocket(key_input, "Key", blender::SOCK_STRING, &key_default);
  InitSocket(keyboard_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(keyboard.inputs, key_input);
  Append(keyboard.outputs, keyboard_out);
  Append(tree.nodes, keyboard);

  blender::bNodeSocketValueString message_default = {};
  SetCString(message_default.value, "pressed");
  blender::bNode print_node = {};
  blender::bNodeSocket print_flow = {};
  blender::bNodeSocket print_message = {};
  InitNode(print_node, "LogicNativePrint", "Print", 2);
  InitSocket(print_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(print_message, "Message", blender::SOCK_STRING, &message_default);
  Append(print_node.inputs, print_flow);
  Append(print_node.inputs, print_message);
  Append(tree.nodes, print_node);

  blender::bNodeLink keyboard_to_print = {};
  InitLink(keyboard_to_print, keyboard, keyboard_out, print_node, print_flow);
  Append(tree.links, keyboard_to_print);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  const LN_Instruction *instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::Print);
  ASSERT_NE(instruction, nullptr);
  ASSERT_NE(instruction->bool_guard_expr_index, LN_INVALID_INDEX);

  const std::vector<LN_Instruction> &instructions = program->GetInstructions(
      LN_Event::OnFixedUpdate);
  const LN_Instruction *route = FindBranchRouteWithConditionKind(
      *program, instructions, LN_BoolExpressionKind::InputStatus);
  ASSERT_NE(route, nullptr);
  const LN_BoolExpression *condition = BoolExpressionAt(*program, route->bool_expr_index);
  ASSERT_NE(condition, nullptr);
  EXPECT_EQ(condition->kind, LN_BoolExpressionKind::InputStatus);
  EXPECT_EQ(condition->int_value, int32_t(SCA_IInputDevice::SPACEKEY));
  EXPECT_EQ(condition->secondary_int_value, int32_t(SCA_InputEvent::JUSTACTIVATED));
  EXPECT_EQ(condition->string_value, "SPACE");
  ExpectInstructionGuardedByRouteCondition(
      *program, LN_Event::OnFixedUpdate, *instruction, LN_BoolExpressionKind::InputStatus);

}

TEST(LN_TreeCompiler, InputActionAndTheAlsoUIFlagCompile)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  /* Input Action "Jump", While Held, Also When UI Uses It. */
  blender::bNodeSocketValueString action_default = {};
  SetCString(action_default.value, "Jump");
  blender::bNodeSocketValueInt user_default = {};
  blender::bNode action = {};
  blender::bNodeSocket action_name = {};
  blender::bNodeSocket action_user = {};
  blender::bNodeSocket action_out = {};
  InitNode(action, "LogicNativeInputAction", "Input Action", 1);
  action.custom1 = int16_t(1 | blender::LOGIC_NODE_INPUT_ALSO_UI);
  InitSocket(action_name, "Action", blender::SOCK_STRING, &action_default);
  InitSocket(action_user, "User", blender::SOCK_INT, &user_default);
  InitSocket(action_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(action.inputs, action_name);
  Append(action.inputs, action_user);
  Append(action.outputs, action_out);
  Append(tree.nodes, action);

  /* Mouse Wheel, Scroll Down, Also When UI Uses It: the flag must not change the direction. */
  blender::bNode wheel = {};
  blender::bNodeSocket wheel_out = {};
  InitNode(wheel, "LogicNativeMouseWheel", "Mouse Wheel", 2);
  wheel.custom1 = int16_t(1 | blender::LOGIC_NODE_INPUT_ALSO_UI);
  InitSocket(wheel_out, "When Scrolled", blender::SOCK_BOOLEAN, nullptr);
  Append(wheel.outputs, wheel_out);
  Append(tree.nodes, wheel);

  blender::bNode quit = {};
  blender::bNodeSocket quit_flow = {};
  InitNode(quit, "LogicNativeQuitGame", "Quit", 3);
  InitSocket(quit_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  Append(quit.inputs, quit_flow);
  Append(tree.nodes, quit);
  blender::bNode restart = {};
  blender::bNodeSocket restart_flow = {};
  InitNode(restart, "LogicNativeRestartGame", "Restart", 4);
  InitSocket(restart_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  Append(restart.inputs, restart_flow);
  Append(tree.nodes, restart);

  blender::bNodeLink action_to_quit = {};
  blender::bNodeLink wheel_to_restart = {};
  InitLink(action_to_quit, action, action_out, quit, quit_flow);
  InitLink(wheel_to_restart, wheel, wheel_out, restart, restart_flow);
  Append(tree.links, action_to_quit);
  Append(tree.links, wheel_to_restart);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  int action_states = 0;
  int wheels = 0;
  for (const LN_BoolExpression &expression : program->GetBoolExpressions()) {
    if (expression.kind == LN_BoolExpressionKind::InputActionState) {
      action_states++;
      EXPECT_EQ(expression.secondary_int_value, int32_t(SCA_InputEvent::ACTIVE));
      EXPECT_TRUE(expression.bool_value);
      const LN_StringExpression *name = StringExpressionAt(*program, expression.input1);
      ASSERT_NE(name, nullptr);
      EXPECT_EQ(name->string_value, "Jump");
    }
    if (expression.kind == LN_BoolExpressionKind::MouseWheelMoved) {
      wheels++;
      EXPECT_EQ(expression.int_value, 2);
      EXPECT_TRUE(expression.bool_value);
    }
  }
  EXPECT_EQ(action_states, 1);
  EXPECT_EQ(wheels, 1);
  const uint32_t channels = program->GetDependencySummary().input_channels;
  EXPECT_NE(channels & LN_DEP_INPUT_ACTION, 0u);
  EXPECT_NE(channels & LN_DEP_INPUT_RAW, 0u);
}

TEST(LN_TreeCompiler, GamepadTriggerSeparatesSelectedEdgeFromActiveState)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNodeSocketValueInt index_default = {};
  blender::bNode gamepad = {};
  blender::bNodeSocket index = {};
  blender::bNodeSocket triggered = {};
  blender::bNodeSocket active = {};
  InitNode(gamepad, "LogicNativeGamepadButton", "Gamepad Trigger", 1);
  gamepad.custom1 = 2;
  gamepad.custom2 = 15;
  InitSocket(index, "Index", blender::SOCK_INT, &index_default);
  InitSocket(triggered, "Out", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(active, "Active", blender::SOCK_BOOLEAN, nullptr);
  Append(gamepad.inputs, index);
  Append(gamepad.outputs, triggered);
  Append(gamepad.outputs, active);
  Append(tree.nodes, gamepad);

  blender::bNode branch = {};
  blender::bNodeSocket branch_flow = {};
  blender::bNodeSocket condition = {};
  blender::bNodeSocket true_out = {};
  blender::bNodeSocket false_out = {};
  InitNode(branch, "LogicNativeBranch", "Branch", 2);
  InitSocket(branch_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(condition, "Condition", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(true_out, "True", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(false_out, "False", blender::SOCK_BOOLEAN, nullptr);
  Append(branch.inputs, branch_flow);
  Append(branch.inputs, condition);
  Append(branch.outputs, true_out);
  Append(branch.outputs, false_out);
  Append(tree.nodes, branch);

  blender::bNode quit = {};
  blender::bNodeSocket quit_flow = {};
  InitNode(quit, "LogicNativeQuitGame", "Quit", 3);
  InitSocket(quit_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  Append(quit.inputs, quit_flow);
  Append(tree.nodes, quit);

  blender::bNodeLink trigger_to_flow = {};
  blender::bNodeLink active_to_condition = {};
  blender::bNodeLink true_to_quit = {};
  InitLink(trigger_to_flow, gamepad, triggered, branch, branch_flow);
  InitLink(active_to_condition, gamepad, active, branch, condition);
  InitLink(true_to_quit, branch, true_out, quit, quit_flow);
  Append(tree.links, trigger_to_flow);
  Append(tree.links, active_to_condition);
  Append(tree.links, true_to_quit);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  int released_expressions = 0;
  int active_expressions = 0;
  for (const LN_BoolExpression &expression : program->GetBoolExpressions()) {
    if (expression.kind != LN_BoolExpressionKind::GamepadButton || !expression.bool_value) {
      continue;
    }
    const LN_IntExpression *status = IntExpressionAt(*program, expression.int_expr_index);
    ASSERT_NE(status, nullptr);
    ASSERT_EQ(status->kind, LN_IntExpressionKind::Constant);
    if (status->int_value == int32_t(SCA_InputEvent::JUSTRELEASED)) {
      released_expressions++;
    }
    else if (status->int_value == int32_t(SCA_InputEvent::ACTIVE)) {
      active_expressions++;
    }
  }
  EXPECT_EQ(released_expressions, 1);
  EXPECT_EQ(active_expressions, 1);
}

TEST(LN_TreeCompiler, KeyboardActiveStateCanBeSampledByFixedSimulationFlow)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_fixed = {};
  blender::bNodeSocket fixed_out = {};
  InitNode(on_fixed, "LogicNativeOnFixedUpdate", "On Logic + Physics Step", 1);
  InitSocket(fixed_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_fixed.outputs, fixed_out);
  Append(tree.nodes, on_fixed);

  blender::bNodeSocketValueString key_default = {};
  SetCString(key_default.value, "SPACE");
  blender::bNode keyboard = {};
  blender::bNodeSocket key = {};
  blender::bNodeSocket active = {};
  InitNode(keyboard, "LogicNativeKeyboardKey", "Keyboard Key", 2);
  keyboard.custom1 = 3; /* Active. */
  InitSocket(key, "Key", blender::SOCK_STRING, &key_default);
  InitSocket(active, "Active", blender::SOCK_BOOLEAN, nullptr);
  Append(keyboard.inputs, key);
  Append(keyboard.outputs, active);
  Append(tree.nodes, keyboard);

  blender::bNode branch = {};
  blender::bNodeSocket branch_flow = {};
  blender::bNodeSocket condition = {};
  blender::bNodeSocket true_out = {};
  InitNode(branch, "LogicNativeBranch", "Branch", 3);
  InitSocket(branch_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(condition, "Condition", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(true_out, "True", blender::SOCK_BOOLEAN, nullptr);
  Append(branch.inputs, branch_flow);
  Append(branch.inputs, condition);
  Append(branch.outputs, true_out);
  Append(tree.nodes, branch);

  blender::bNodeSocketValueString message_default = {};
  SetCString(message_default.value, "held");
  blender::bNode print_node = {};
  blender::bNodeSocket print_flow = {};
  blender::bNodeSocket message = {};
  InitNode(print_node, "LogicNativePrint", "Print", 4);
  InitSocket(print_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(message, "Message", blender::SOCK_STRING, &message_default);
  Append(print_node.inputs, print_flow);
  Append(print_node.inputs, message);
  Append(tree.nodes, print_node);

  blender::bNodeLink fixed_to_branch = {};
  blender::bNodeLink active_to_condition = {};
  blender::bNodeLink true_to_print = {};
  InitLink(fixed_to_branch, on_fixed, fixed_out, branch, branch_flow);
  InitLink(active_to_condition, keyboard, active, branch, condition);
  InitLink(true_to_print, branch, true_out, print_node, print_flow);
  Append(tree.links, fixed_to_branch);
  Append(tree.links, active_to_condition);
  Append(tree.links, true_to_print);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  const LN_Instruction *print = FindInstruction(program->GetInstructions(LN_Event::OnFixedUpdate),
                                                LN_OpCode::Print);
  ASSERT_NE(print, nullptr);
  const std::vector<LN_Instruction> &instructions = program->GetInstructions(
      LN_Event::OnFixedUpdate);
  const LN_Instruction *route = FindBranchRouteWithConditionKind(
      *program, instructions, LN_BoolExpressionKind::InputStatus);
  ASSERT_NE(route, nullptr);
  ExpectInstructionGuardedByRouteCondition(
      *program, LN_Event::OnFixedUpdate, *print, LN_BoolExpressionKind::InputStatus);
}

TEST(LN_TreeCompiler, KeyboardPressedConditionUsesFixedInputEdgeIndependentlyOfTriggerMode)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_fixed = {};
  blender::bNodeSocket fixed_out = {};
  InitNode(on_fixed, "LogicNativeOnFixedUpdate", "On Logic + Physics Step", 1);
  InitSocket(fixed_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_fixed.outputs, fixed_out);
  Append(tree.nodes, on_fixed);

  blender::bNodeSocketValueString key_default = {};
  SetCString(key_default.value, "SPACE");
  blender::bNode keyboard = {};
  blender::bNodeSocket key = {};
  blender::bNodeSocket pressed = {};
  InitNode(keyboard, "LogicNativeKeyboardKey", "Keyboard Key", 2);
  keyboard.custom1 = 2; /* Triggered is configured for Released; Pressed remains explicit. */
  InitSocket(key, "Key", blender::SOCK_STRING, &key_default);
  InitSocket(pressed, "Pressed", blender::SOCK_BOOLEAN, nullptr);
  Append(keyboard.inputs, key);
  Append(keyboard.outputs, pressed);
  Append(tree.nodes, keyboard);

  blender::bNode branch = {};
  blender::bNodeSocket branch_flow = {};
  blender::bNodeSocket condition = {};
  blender::bNodeSocket true_out = {};
  InitNode(branch, "LogicNativeBranch", "Branch", 3);
  InitSocket(branch_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(condition, "Condition", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(true_out, "True", blender::SOCK_BOOLEAN, nullptr);
  Append(branch.inputs, branch_flow);
  Append(branch.inputs, condition);
  Append(branch.outputs, true_out);
  Append(tree.nodes, branch);

  blender::bNode print_node = {};
  blender::bNodeSocket print_flow = {};
  blender::bNodeSocketValueString message_default = {};
  blender::bNodeSocket message = {};
  SetCString(message_default.value, "pressed");
  InitNode(print_node, "LogicNativePrint", "Print", 4);
  InitSocket(print_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(message, "Message", blender::SOCK_STRING, &message_default);
  Append(print_node.inputs, print_flow);
  Append(print_node.inputs, message);
  Append(tree.nodes, print_node);

  blender::bNodeLink fixed_to_branch = {};
  blender::bNodeLink pressed_to_condition = {};
  blender::bNodeLink true_to_print = {};
  InitLink(fixed_to_branch, on_fixed, fixed_out, branch, branch_flow);
  InitLink(pressed_to_condition, keyboard, pressed, branch, condition);
  InitLink(true_to_print, branch, true_out, print_node, print_flow);
  Append(tree.links, fixed_to_branch);
  Append(tree.links, pressed_to_condition);
  Append(tree.links, true_to_print);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  const std::vector<LN_Instruction> &instructions = program->GetInstructions(
      LN_Event::OnFixedUpdate);
  const LN_Instruction *route = FindBranchRouteWithConditionKind(
      *program, instructions, LN_BoolExpressionKind::InputStatus);
  ASSERT_NE(route, nullptr);
  const LN_BoolExpression *condition_expression = BoolExpressionAt(*program,
                                                                   route->bool_expr_index);
  ASSERT_NE(condition_expression, nullptr);
  EXPECT_EQ(condition_expression->secondary_int_value,
            int32_t(SCA_InputEvent::JUSTACTIVATED));
}

TEST(LN_TreeCompiler, MouseWheelSetTimeScaleRunsOnFixedLogicWhilePhysicsIsPaused)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode mouse_wheel = {};
  blender::bNodeSocket when_scrolled = {};
  InitNode(mouse_wheel, "LogicNativeMouseWheel", "Mouse Wheel", 1);
  InitSocket(when_scrolled, "When Scrolled", blender::SOCK_BOOLEAN, nullptr);
  mouse_wheel.custom1 = 0;
  Append(mouse_wheel.outputs, when_scrolled);
  Append(tree.nodes, mouse_wheel);

  blender::bNodeSocketValueFloat timescale_default = {};
  timescale_default.value = 1.25f;
  blender::bNode set_timescale = {};
  blender::bNodeSocket timescale_flow = {};
  blender::bNodeSocket timescale = {};
  InitNode(set_timescale, "LogicNativeSetTimescale", "Set Timescale", 2);
  InitSocket(timescale_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(timescale, "Timescale", blender::SOCK_FLOAT, &timescale_default);
  Append(set_timescale.inputs, timescale_flow);
  Append(set_timescale.inputs, timescale);
  Append(tree.nodes, set_timescale);

  blender::bNodeLink wheel_to_timescale = {};
  InitLink(wheel_to_timescale, mouse_wheel, when_scrolled, set_timescale, timescale_flow);
  Append(tree.links, wheel_to_timescale);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  const LN_Instruction *instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::SetTimeScale);
  ASSERT_NE(instruction, nullptr);
  EXPECT_EQ(
      FindInstruction(program->GetInstructions(LN_Event::OnUpdate), LN_OpCode::SetTimeScale),
      nullptr);

  const std::vector<LN_Instruction> &instructions = program->GetInstructions(
      LN_Event::OnFixedUpdate);
  const LN_Instruction *route = FindBranchRouteWithConditionKind(
      *program, instructions, LN_BoolExpressionKind::MouseWheelMoved);
  ASSERT_NE(route, nullptr);
  ExpectInstructionGuardedByRouteCondition(
      *program, LN_Event::OnFixedUpdate, *instruction, LN_BoolExpressionKind::MouseWheelMoved);
}

TEST(LN_TreeCompiler, PauseNodesCompileAsGlobalBooleanCommandsInTheirIntendedPhases)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket update_out = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(update_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, update_out);
  Append(tree.nodes, on_update);

  blender::bNode on_fixed_update = {};
  blender::bNodeSocket fixed_update_out = {};
  InitNode(on_fixed_update, "LogicNativeOnFixedUpdate", "On Fixed Update", 2);
  InitSocket(fixed_update_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_fixed_update.outputs, fixed_update_out);
  Append(tree.nodes, on_fixed_update);

  blender::bNodeSocketValueBoolean game_paused_default = {};
  game_paused_default.value = true;
  blender::bNode set_game_paused = {};
  blender::bNodeSocket game_paused_flow = {};
  blender::bNodeSocket game_paused = {};
  InitNode(set_game_paused, "LogicNativeSetGamePaused", "Set Game Paused", 3);
  InitSocket(game_paused_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(game_paused, "Paused", blender::SOCK_BOOLEAN, &game_paused_default);
  Append(set_game_paused.inputs, game_paused_flow);
  Append(set_game_paused.inputs, game_paused);
  Append(tree.nodes, set_game_paused);

  blender::bNodeSocketValueBoolean physics_paused_default = {};
  physics_paused_default.value = false;
  blender::bNode set_physics_paused = {};
  blender::bNodeSocket physics_paused_flow = {};
  blender::bNodeSocket physics_paused = {};
  InitNode(set_physics_paused, "LogicNativeSetPhysicsPaused", "Set Physics Paused", 4);
  InitSocket(physics_paused_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(physics_paused, "Paused", blender::SOCK_BOOLEAN, &physics_paused_default);
  Append(set_physics_paused.inputs, physics_paused_flow);
  Append(set_physics_paused.inputs, physics_paused);
  Append(tree.nodes, set_physics_paused);

  blender::bNodeLink update_to_game_pause = {};
  InitLink(update_to_game_pause, on_update, update_out, set_game_paused, game_paused_flow);
  Append(tree.links, update_to_game_pause);

  blender::bNodeLink fixed_update_to_physics_pause = {};
  InitLink(fixed_update_to_physics_pause,
           on_fixed_update,
           fixed_update_out,
           set_physics_paused,
           physics_paused_flow);
  Append(tree.links, fixed_update_to_physics_pause);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  const LN_Instruction *game_pause_instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnUpdate), LN_OpCode::SetGamePaused);
  ASSERT_NE(game_pause_instruction, nullptr);
  EXPECT_EQ(game_pause_instruction->value_expr_index, LN_INVALID_INDEX);
  const LN_BoolExpression *game_pause_value = BoolExpressionAt(
      *program, game_pause_instruction->bool_expr_index);
  ASSERT_NE(game_pause_value, nullptr);
  EXPECT_EQ(game_pause_value->kind, LN_BoolExpressionKind::Constant);
  EXPECT_TRUE(game_pause_value->bool_value);

  const LN_Instruction *physics_pause_instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::SetPhysicsPaused);
  ASSERT_NE(physics_pause_instruction, nullptr);
  EXPECT_EQ(physics_pause_instruction->value_expr_index, LN_INVALID_INDEX);
  const LN_BoolExpression *physics_pause_value = BoolExpressionAt(
      *program, physics_pause_instruction->bool_expr_index);
  ASSERT_NE(physics_pause_value, nullptr);
  EXPECT_EQ(physics_pause_value->kind, LN_BoolExpressionKind::Constant);
  EXPECT_FALSE(physics_pause_value->bool_value);

  EXPECT_EQ(
      FindInstruction(program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::SetGamePaused),
      nullptr);
  EXPECT_EQ(
      FindInstruction(program->GetInstructions(LN_Event::OnUpdate), LN_OpCode::SetPhysicsPaused),
      nullptr);
}

TEST(LN_TreeCompiler, PauseNodeDoneContinuesInTheSameFrameUpdate)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket update_out = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(update_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, update_out);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueBoolean game_paused_default = {};
  game_paused_default.value = true;
  blender::bNode set_game_paused = {};
  blender::bNodeSocket game_flow = {};
  blender::bNodeSocket game_paused = {};
  blender::bNodeSocket game_done = {};
  InitNode(set_game_paused, "LogicNativeSetGamePaused", "Set Game Paused", 2);
  InitSocket(game_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(game_paused, "Paused", blender::SOCK_BOOLEAN, &game_paused_default);
  InitSocket(game_done, "Done", blender::SOCK_BOOLEAN, nullptr);
  Append(set_game_paused.inputs, game_flow);
  Append(set_game_paused.inputs, game_paused);
  Append(set_game_paused.outputs, game_done);
  Append(tree.nodes, set_game_paused);

  blender::bNodeSocketValueBoolean physics_paused_default = {};
  physics_paused_default.value = true;
  blender::bNode set_physics_paused = {};
  blender::bNodeSocket physics_flow = {};
  blender::bNodeSocket physics_paused = {};
  InitNode(set_physics_paused, "LogicNativeSetPhysicsPaused", "Set Physics Paused", 3);
  InitSocket(physics_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(physics_paused, "Paused", blender::SOCK_BOOLEAN, &physics_paused_default);
  Append(set_physics_paused.inputs, physics_flow);
  Append(set_physics_paused.inputs, physics_paused);
  Append(tree.nodes, set_physics_paused);

  blender::bNodeLink update_to_game = {};
  blender::bNodeLink game_to_physics = {};
  InitLink(update_to_game, on_update, update_out, set_game_paused, game_flow);
  InitLink(game_to_physics, set_game_paused, game_done, set_physics_paused, physics_flow);
  Append(tree.links, update_to_game);
  Append(tree.links, game_to_physics);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  const std::vector<LN_Instruction> &instructions = program->GetInstructions(LN_Event::OnUpdate);
  const LN_Instruction *set_game = FindInstruction(instructions, LN_OpCode::SetGamePaused);
  const LN_Instruction *set_physics = FindInstruction(instructions, LN_OpCode::SetPhysicsPaused);
  ASSERT_NE(set_game, nullptr);
  ASSERT_NE(set_physics, nullptr);
  const LN_BoolExpression *guard = BoolExpressionAt(*program,
                                                    set_physics->bool_guard_expr_index);
  ASSERT_NE(guard, nullptr);
  EXPECT_EQ(guard->kind, LN_BoolExpressionKind::InstructionExecuted);
}

TEST(LN_TreeCompiler, OverlayCollectionNodesRequireExplicitValidTargets)
{
  blender::Collection collection = {};
  SetCString(collection.id.name, "COOverlayUI");
  blender::bNodeSocketValueCollection collection_default = {};
  collection_default.value = &collection;

  blender::bNodeTree missing_camera_tree = {};
  InitTree(missing_camera_tree);
  blender::bNode on_update = {};
  blender::bNodeSocket update_out = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(update_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, update_out);
  Append(missing_camera_tree.nodes, on_update);

  blender::bNode set_overlay = {};
  blender::bNodeSocket set_flow = {};
  blender::bNodeSocket camera = {};
  blender::bNodeSocket set_collection = {};
  InitNode(set_overlay, "LogicNativeSetOverlayCollection", "Set Overlay Collection", 2);
  InitSocket(set_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(camera, "Camera", blender::SOCK_OBJECT, nullptr);
  InitSocket(set_collection,
             "Collection",
             blender::SOCK_COLLECTION,
             &collection_default);
  Append(set_overlay.inputs, set_flow);
  Append(set_overlay.inputs, camera);
  Append(set_overlay.inputs, set_collection);
  Append(missing_camera_tree.nodes, set_overlay);
  blender::bNodeLink set_link = {};
  InitLink(set_link, on_update, update_out, set_overlay, set_flow);
  Append(missing_camera_tree.links, set_link);

  const std::shared_ptr<LN_Program> missing_camera_program = LN_TreeCompiler().Compile(
      missing_camera_tree);
  EXPECT_FALSE(missing_camera_program->GetCompileReport().HasErrors())
      << FormatCompileReport(missing_camera_program->GetCompileReport());
  ASSERT_TRUE(missing_camera_program->ValidateInstructionPayloads());
  const LN_Instruction *empty_camera = FindInstruction(
      missing_camera_program->GetInstructions(LN_Event::OnUpdate),
      LN_OpCode::SetOverlayCollection);
  ASSERT_NE(empty_camera, nullptr);
  EXPECT_EQ(empty_camera->value_expr_index, LN_INVALID_INDEX);
  EXPECT_NE(empty_camera->property_ref_index, LN_INVALID_INDEX);

  blender::bNodeTree missing_collection_tree = {};
  InitTree(missing_collection_tree);
  blender::bNode remove_overlay = {};
  blender::bNodeSocket remove_flow = {};
  blender::bNodeSocket remove_collection = {};
  InitNode(remove_overlay,
           "LogicNativeRemoveOverlayCollection",
           "Remove Overlay Collection",
           3);
  InitSocket(remove_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(remove_collection, "Collection", blender::SOCK_COLLECTION, nullptr);
  Append(remove_overlay.inputs, remove_flow);
  Append(remove_overlay.inputs, remove_collection);
  Append(missing_collection_tree.nodes, on_update);
  Append(missing_collection_tree.nodes, remove_overlay);
  blender::bNodeLink remove_link = {};
  InitLink(remove_link, on_update, update_out, remove_overlay, remove_flow);
  Append(missing_collection_tree.links, remove_link);

  const std::shared_ptr<LN_Program> missing_collection_program = LN_TreeCompiler().Compile(
      missing_collection_tree);
  EXPECT_TRUE(missing_collection_program->GetCompileReport().HasErrors());
  EXPECT_EQ(FindInstruction(missing_collection_program->GetInstructions(LN_Event::OnUpdate),
                            LN_OpCode::RemoveOverlayCollection),
            nullptr);
}

TEST(LN_TreeCompiler, SetCollectionRenderVisibilityRequiresACollection)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket update_out = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(update_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, update_out);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueBoolean visible_default = {};
  visible_default.value = true;
  blender::bNodeSocketValueBoolean children_default = {};
  blender::bNode set_visibility = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket collection = {};
  blender::bNodeSocket visible = {};
  blender::bNodeSocket include_children = {};
  InitNode(set_visibility,
           "LogicNativeSetCollectionVisibility",
           "Set Collection Render Visibility",
           2);
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(collection, "Collection", blender::SOCK_COLLECTION, nullptr);
  InitSocket(visible, "Visible", blender::SOCK_BOOLEAN, &visible_default);
  InitSocket(include_children,
             "Include Child Collections",
             blender::SOCK_BOOLEAN,
             &children_default);
  Append(set_visibility.inputs, flow);
  Append(set_visibility.inputs, collection);
  Append(set_visibility.inputs, visible);
  Append(set_visibility.inputs, include_children);
  Append(tree.nodes, set_visibility);

  blender::bNodeLink flow_link = {};
  InitLink(flow_link, on_update, update_out, set_visibility, flow);
  Append(tree.links, flow_link);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_TRUE(program->GetCompileReport().HasErrors());
  EXPECT_EQ(FindInstruction(program->GetInstructions(LN_Event::OnUpdate),
                            LN_OpCode::SetCollectionVisibility),
            nullptr);
  EXPECT_NE(FormatCompileReport(program->GetCompileReport()).find("requires a collection"),
            std::string::npos);
}

TEST(LN_TreeCompiler, SetOverlayCollectionRejectsStaticNonCameraObject)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::Object mesh_object = {};
  SetCString(mesh_object.id.name, "OBNotACamera");
  mesh_object.type = blender::OB_MESH;
  blender::bNodeSocketValueObject camera_default = {};
  camera_default.value = &mesh_object;

  blender::Collection collection = {};
  SetCString(collection.id.name, "COOverlayUI");
  blender::bNodeSocketValueCollection collection_default = {};
  collection_default.value = &collection;

  blender::bNode on_update = {};
  blender::bNodeSocket update_out = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(update_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, update_out);
  Append(tree.nodes, on_update);

  blender::bNode set_overlay = {};
  blender::bNodeSocket set_flow = {};
  blender::bNodeSocket camera = {};
  blender::bNodeSocket set_collection = {};
  InitNode(set_overlay, "LogicNativeSetOverlayCollection", "Set Overlay Collection", 2);
  InitSocket(set_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(camera, "Camera", blender::SOCK_OBJECT, &camera_default);
  InitSocket(set_collection,
             "Collection",
             blender::SOCK_COLLECTION,
             &collection_default);
  Append(set_overlay.inputs, set_flow);
  Append(set_overlay.inputs, camera);
  Append(set_overlay.inputs, set_collection);
  Append(tree.nodes, set_overlay);

  blender::bNodeLink set_link = {};
  InitLink(set_link, on_update, update_out, set_overlay, set_flow);
  Append(tree.links, set_link);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_TRUE(program->GetCompileReport().HasErrors());
  EXPECT_EQ(FindInstruction(program->GetInstructions(LN_Event::OnUpdate),
                            LN_OpCode::SetOverlayCollection),
            nullptr);
  EXPECT_NE(FormatCompileReport(program->GetCompileReport()).find("must be a Camera object"),
            std::string::npos);
}

TEST(LN_TreeCompiler, OverlayCollectionDoneChainsSetAndRemoveWithDedicatedCamera)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::Object camera_object = {};
  SetCString(camera_object.id.name, "OBOverlayCamera");
  camera_object.type = blender::OB_CAMERA;
  blender::bNodeSocketValueObject camera_default = {};
  camera_default.value = &camera_object;
  blender::Collection collection = {};
  SetCString(collection.id.name, "COOverlayUI");
  blender::bNodeSocketValueCollection collection_default = {};
  collection_default.value = &collection;

  blender::bNode on_update = {};
  blender::bNodeSocket update_out = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(update_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, update_out);
  Append(tree.nodes, on_update);

  blender::bNode set_overlay = {};
  blender::bNodeSocket set_flow = {};
  blender::bNodeSocket camera = {};
  blender::bNodeSocket set_collection = {};
  blender::bNodeSocket set_success = {};
  InitNode(set_overlay, "LogicNativeSetOverlayCollection", "Set Overlay Collection", 2);
  InitSocket(set_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(camera, "Camera", blender::SOCK_OBJECT, &camera_default);
  InitSocket(set_collection,
             "Collection",
             blender::SOCK_COLLECTION,
             &collection_default);
  InitSocket(set_success, "Success", blender::SOCK_BOOLEAN, nullptr);
  Append(set_overlay.inputs, set_flow);
  Append(set_overlay.inputs, camera);
  Append(set_overlay.inputs, set_collection);
  Append(set_overlay.outputs, set_success);
  Append(tree.nodes, set_overlay);

  blender::bNode remove_overlay = {};
  blender::bNodeSocket remove_flow = {};
  blender::bNodeSocket remove_collection = {};
  InitNode(remove_overlay,
           "LogicNativeRemoveOverlayCollection",
           "Remove Overlay Collection",
           3);
  InitSocket(remove_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(remove_collection,
             "Collection",
             blender::SOCK_COLLECTION,
             &collection_default);
  Append(remove_overlay.inputs, remove_flow);
  Append(remove_overlay.inputs, remove_collection);
  Append(tree.nodes, remove_overlay);

  blender::bNodeLink update_to_set = {};
  blender::bNodeLink set_to_remove = {};
  InitLink(update_to_set, on_update, update_out, set_overlay, set_flow);
  InitLink(set_to_remove, set_overlay, set_success, remove_overlay, remove_flow);
  Append(tree.links, update_to_set);
  Append(tree.links, set_to_remove);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  const std::vector<LN_Instruction> &instructions = program->GetInstructions(LN_Event::OnUpdate);
  const LN_Instruction *set_instruction = FindInstruction(
      instructions, LN_OpCode::SetOverlayCollection);
  const LN_Instruction *remove_instruction = FindInstruction(
      instructions, LN_OpCode::RemoveOverlayCollection);
  ASSERT_NE(set_instruction, nullptr);
  ASSERT_NE(remove_instruction, nullptr);
  const LN_ValueExpression *camera_expression = ValueExpressionAt(
      *program, set_instruction->value_expr_index);
  ASSERT_NE(camera_expression, nullptr);
  EXPECT_EQ(camera_expression->kind, LN_ValueExpressionKind::Constant);
  EXPECT_EQ(camera_expression->value.type, LN_ValueType::ObjectRef);
  EXPECT_NE(set_instruction->property_ref_index, LN_INVALID_INDEX);
  EXPECT_EQ(program->GetBoolExpressions()[set_instruction->property_ref_index].kind,
            LN_BoolExpressionKind::UICommandCompleted);
  const LN_BoolExpression *remove_guard = BoolExpressionAt(
      *program, remove_instruction->bool_guard_expr_index);
  ASSERT_NE(remove_guard, nullptr);
  EXPECT_EQ(remove_guard->kind, LN_BoolExpressionKind::UICommandSucceeded);
}

TEST(LN_TreeCompiler, SceneTransitionDoneChainsPreserveTheOwningEventRoute)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket update_out = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(update_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, update_out);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueString load_name = {};
  SetCString(load_name.value, "SharedObjects");
  blender::bNode load_scene = {};
  blender::bNodeSocket load_flow = {};
  blender::bNodeSocket load_scene_name = {};
  blender::bNodeSocket load_done = {};
  InitNode(load_scene, "LogicNativeLoadScene", "Load Scene", 2);
  InitSocket(load_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(load_scene_name, "Scene", blender::SOCK_STRING, &load_name);
  InitSocket(load_done, "Done", blender::SOCK_BOOLEAN, nullptr);
  Append(load_scene.inputs, load_flow);
  Append(load_scene.inputs, load_scene_name);
  Append(load_scene.outputs, load_done);
  Append(tree.nodes, load_scene);

  blender::bNodeSocketValueString set_name = {};
  SetCString(set_name.value, "MainMenu");
  blender::bNode set_scene = {};
  blender::bNodeSocket set_flow = {};
  blender::bNodeSocket set_scene_name = {};
  InitNode(set_scene, "LogicNativeSetScene", "Set Scene", 3);
  InitSocket(set_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(set_scene_name, "Scene", blender::SOCK_STRING, &set_name);
  Append(set_scene.inputs, set_flow);
  Append(set_scene.inputs, set_scene_name);
  Append(tree.nodes, set_scene);

  blender::bNodeLink update_to_load = {};
  blender::bNodeLink load_to_set = {};
  InitLink(update_to_load, on_update, update_out, load_scene, load_flow);
  InitLink(load_to_set, load_scene, load_done, set_scene, set_flow);
  Append(tree.links, update_to_load);
  Append(tree.links, load_to_set);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  const std::vector<LN_Instruction> &instructions = program->GetInstructions(LN_Event::OnUpdate);
  const LN_Instruction *load_instruction = FindInstruction(instructions, LN_OpCode::LoadScene);
  const LN_Instruction *set_instruction = FindInstruction(instructions, LN_OpCode::SetScene);
  ASSERT_NE(load_instruction, nullptr);
  ASSERT_NE(set_instruction, nullptr);
  const LN_BoolExpression *guard = BoolExpressionAt(*program,
                                                    set_instruction->bool_guard_expr_index);
  ASSERT_NE(guard, nullptr);
  EXPECT_EQ(guard->kind, LN_BoolExpressionKind::InstructionExecuted);
}

TEST(LN_TreeCompiler, WhilePausedCompilesStaticScopeGuardAndUnscaledDelta)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode while_paused = {};
  blender::bNodeSocket object = {};
  blender::bNodeSocket collection = {};
  blender::bNodeSocket update = {};
  blender::bNodeSocket delta_time = {};
  InitNode(while_paused, "LogicNativeWhilePaused", "While Paused", 1);
  while_paused.custom1 = int(LN_PauseProcessingScope::Owner);
  InitSocket(object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(collection, "Collection", blender::SOCK_COLLECTION, nullptr);
  InitSocket(update, "Update", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(delta_time, "Delta Time", blender::SOCK_FLOAT, nullptr);
  Append(while_paused.inputs, object);
  Append(while_paused.inputs, collection);
  Append(while_paused.outputs, update);
  Append(while_paused.outputs, delta_time);
  Append(tree.nodes, while_paused);

  blender::bNode set_timescale = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket timescale = {};
  InitNode(set_timescale, "LogicNativeSetTimescale", "Set Timescale", 2);
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(timescale, "Timescale", blender::SOCK_FLOAT, nullptr);
  Append(set_timescale.inputs, flow);
  Append(set_timescale.inputs, timescale);
  Append(tree.nodes, set_timescale);

  blender::bNodeLink update_link = {};
  blender::bNodeLink delta_link = {};
  InitLink(update_link, while_paused, update, set_timescale, flow);
  InitLink(delta_link, while_paused, delta_time, set_timescale, timescale);
  Append(tree.links, update_link);
  Append(tree.links, delta_link);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  ASSERT_EQ(program->GetPauseProcessingRequests().size(), 1u);
  EXPECT_EQ(program->GetPauseProcessingRequests()[0].scope, LN_PauseProcessingScope::Owner);
  EXPECT_EQ(program->GetPauseProcessingRequests()[0].target_value_expr_index, LN_INVALID_INDEX);

  const LN_Instruction *instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnUpdate), LN_OpCode::SetTimeScale);
  ASSERT_NE(instruction, nullptr);
  const LN_BoolExpression *guard = BoolExpressionAt(*program,
                                                    instruction->bool_guard_expr_index);
  ASSERT_NE(guard, nullptr);
  EXPECT_EQ(guard->kind, LN_BoolExpressionKind::GamePaused);
  const LN_FloatExpression *delta = FloatExpressionAt(*program,
                                                      instruction->float_expr_index);
  ASSERT_NE(delta, nullptr);
  EXPECT_EQ(delta->kind, LN_FloatExpressionKind::UnscaledDeltaTime);
}

TEST(LN_TreeCompiler, SetPhysicsStateCompilesExplicitJoltMotionModes)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket update_out = {};
  InitNode(on_update, "LogicNativeOnFixedUpdate", "On Fixed Update", 1);
  InitSocket(update_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, update_out);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueBoolean enabled_default = {};
  enabled_default.value = true;
  blender::bNode set_restore_original = {};
  blender::bNode set_dynamic = {};
  blender::bNode set_rigid_body = {};
  blender::bNode set_kinematic = {};
  blender::bNodeSocket restore_original_flow = {};
  blender::bNodeSocket dynamic_flow = {};
  blender::bNodeSocket rigid_body_flow = {};
  blender::bNodeSocket kinematic_flow = {};
  blender::bNodeSocket restore_original_object = {};
  blender::bNodeSocket dynamic_object = {};
  blender::bNodeSocket rigid_body_object = {};
  blender::bNodeSocket kinematic_object = {};
  blender::bNodeSocket restore_original_enabled = {};
  blender::bNodeSocket dynamic_enabled = {};
  blender::bNodeSocket rigid_body_enabled = {};
  blender::bNodeSocket kinematic_enabled = {};

  const auto initialize_set_physics_state = [&](blender::bNode &node,
                                                blender::bNodeSocket &flow,
                                                blender::bNodeSocket &object,
                                                blender::bNodeSocket &enabled,
                                                const int32_t identifier,
                                                const int32_t mode) {
    InitNode(node, "LogicNativeSetDynamics", "Set Physics State", identifier);
    node.custom1 = mode;
    InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
    InitSocket(object, "Object", blender::SOCK_OBJECT, nullptr);
    InitSocket(enabled, "Enabled", blender::SOCK_BOOLEAN, &enabled_default);
    Append(node.inputs, flow);
    Append(node.inputs, object);
    Append(node.inputs, enabled);
    Append(tree.nodes, node);
  };

  initialize_set_physics_state(set_restore_original,
                               restore_original_flow,
                               restore_original_object,
                               restore_original_enabled,
                               2,
                               4);
  initialize_set_physics_state(set_dynamic, dynamic_flow, dynamic_object, dynamic_enabled, 3, 5);
  initialize_set_physics_state(
      set_rigid_body, rigid_body_flow, rigid_body_object, rigid_body_enabled, 4, 6);
  initialize_set_physics_state(
      set_kinematic, kinematic_flow, kinematic_object, kinematic_enabled, 5, 7);

  blender::bNodeLink restore_original_link = {};
  blender::bNodeLink dynamic_link = {};
  blender::bNodeLink rigid_body_link = {};
  blender::bNodeLink kinematic_link = {};
  InitLink(
      restore_original_link, on_update, update_out, set_restore_original, restore_original_flow);
  InitLink(dynamic_link, on_update, update_out, set_dynamic, dynamic_flow);
  InitLink(rigid_body_link, on_update, update_out, set_rigid_body, rigid_body_flow);
  InitLink(kinematic_link, on_update, update_out, set_kinematic, kinematic_flow);
  Append(tree.links, restore_original_link);
  Append(tree.links, dynamic_link);
  Append(tree.links, rigid_body_link);
  Append(tree.links, kinematic_link);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  bool found_mode[8] = {};
  for (const LN_Instruction &instruction : program->GetInstructions(LN_Event::OnFixedUpdate)) {
    if (instruction.opcode == LN_OpCode::SetDynamics && instruction.int_value >= 4 &&
        instruction.int_value <= 7)
    {
      found_mode[instruction.int_value] = true;
      EXPECT_EQ(instruction.bool_expr_index, LN_INVALID_INDEX);
    }
  }
  EXPECT_TRUE(found_mode[4]);
  EXPECT_TRUE(found_mode[5]);
  EXPECT_TRUE(found_mode[6]);
  EXPECT_TRUE(found_mode[7]);
}

TEST(LN_TreeCompiler, KeyboardKeyAcceptsBlenderEventIdentifiers)
{
  struct Case {
    const char *key_name;
    SCA_IInputDevice::SCA_EnumInputs expected_code;
  };

  const Case cases[] = {
      {"RET", SCA_IInputDevice::RETKEY},
      {"LEFT_ARROW", SCA_IInputDevice::LEFTARROWKEY},
      {"BACK_SPACE", SCA_IInputDevice::BACKSPACEKEY},
      {"LEFT_SHIFT", SCA_IInputDevice::LEFTSHIFTKEY},
      {"NUMPAD_1", SCA_IInputDevice::PAD1},
      {"NUMPAD_ASTERIX", SCA_IInputDevice::PADASTERKEY},
      {"bge.events.PADSLASHKEY", SCA_IInputDevice::PADSLASHKEY},
      {"F12", SCA_IInputDevice::F12KEY},
  };

  for (const Case &test_case : cases) {
    SCOPED_TRACE(test_case.key_name);

    blender::bNodeTree tree = {};
    InitTree(tree);

    blender::bNodeSocketValueString key_default = {};
    SetCString(key_default.value, test_case.key_name);
    blender::bNode keyboard = {};
    blender::bNodeSocket key_input = {};
    blender::bNodeSocket keyboard_out = {};
    InitNode(keyboard, "LogicNativeKeyboardKey", "Keyboard Key", 1);
    InitSocket(key_input, "Key", blender::SOCK_STRING, &key_default);
    InitSocket(keyboard_out, "Out", blender::SOCK_BOOLEAN, nullptr);
    Append(keyboard.inputs, key_input);
    Append(keyboard.outputs, keyboard_out);
    Append(tree.nodes, keyboard);

    blender::bNodeSocketValueString message_default = {};
    SetCString(message_default.value, "pressed");
    blender::bNode print_node = {};
    blender::bNodeSocket print_flow = {};
    blender::bNodeSocket print_message = {};
    InitNode(print_node, "LogicNativePrint", "Print", 2);
    InitSocket(print_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
    InitSocket(print_message, "Message", blender::SOCK_STRING, &message_default);
    Append(print_node.inputs, print_flow);
    Append(print_node.inputs, print_message);
    Append(tree.nodes, print_node);

    blender::bNodeLink keyboard_to_print = {};
    InitLink(keyboard_to_print, keyboard, keyboard_out, print_node, print_flow);
    Append(tree.links, keyboard_to_print);

    const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
    EXPECT_FALSE(program->GetCompileReport().HasErrors())
        << FormatCompileReport(program->GetCompileReport());

    const LN_Instruction *instruction = FindInstruction(
        program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::Print);
    ASSERT_NE(instruction, nullptr);
    ASSERT_NE(instruction->bool_guard_expr_index, LN_INVALID_INDEX);

    const std::vector<LN_Instruction> &instructions = program->GetInstructions(
        LN_Event::OnFixedUpdate);
    const LN_Instruction *route = FindBranchRouteWithConditionKind(
        *program, instructions, LN_BoolExpressionKind::InputStatus);
    ASSERT_NE(route, nullptr);
    const LN_BoolExpression *condition = BoolExpressionAt(*program, route->bool_expr_index);
    ASSERT_NE(condition, nullptr);
    EXPECT_EQ(condition->kind, LN_BoolExpressionKind::InputStatus);
    EXPECT_EQ(condition->int_value, int32_t(test_case.expected_code));
    ExpectInstructionGuardedByRouteCondition(
        *program, LN_Event::OnFixedUpdate, *instruction, LN_BoolExpressionKind::InputStatus);
  }
}

TEST(LN_TreeCompiler, MouseLookCompilesMouseInputAndCenterMouseBit)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnFixedUpdate", "On Fixed Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueVector invert_default = {};
  invert_default.value[0] = 1.0f;
  invert_default.value[1] = 1.0f;
  blender::bNodeSocketValueFloat sensitivity_default = {};
  sensitivity_default.value = 1000.0f;
  blender::bNodeSocketValueBoolean cap_x_default = {};
  cap_x_default.value = false;
  blender::bNodeSocketValueVector cap_x_range_default = {};
  blender::bNodeSocketValueBoolean cap_y_default = {};
  cap_y_default.value = true;
  blender::bNodeSocketValueVector cap_y_range_default = {};
  cap_y_range_default.value[0] = -1.57079637f;
  cap_y_range_default.value[1] = 1.57079637f;
  blender::bNodeSocketValueFloat smoothing_default = {};
  smoothing_default.value = 0.0f;

  blender::bNode mouse_look = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket body = {};
  blender::bNodeSocket head = {};
  blender::bNodeSocket inverted = {};
  blender::bNodeSocket sensitivity = {};
  blender::bNodeSocket cap_x = {};
  blender::bNodeSocket cap_x_range = {};
  blender::bNodeSocket cap_y = {};
  blender::bNodeSocket cap_y_range = {};
  blender::bNodeSocket smoothing = {};
  InitNode(mouse_look, "LogicNativeMouseLook", "Mouse Look", 2);
  mouse_look.custom1 = 0;
  mouse_look.custom2 = 2 | 4;
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(body, "Body", blender::SOCK_OBJECT, nullptr);
  InitSocket(head, "Head", blender::SOCK_OBJECT, nullptr);
  InitSocket(inverted, "Inverted", blender::SOCK_VECTOR, &invert_default);
  InitSocket(sensitivity, "Sensitivity", blender::SOCK_FLOAT, &sensitivity_default);
  InitSocket(cap_x, "Cap Left / Right", blender::SOCK_BOOLEAN, &cap_x_default);
  InitSocket(cap_x_range,
             "Cap Left / Right Range",
             blender::SOCK_VECTOR,
             &cap_x_range_default,
             "NodeSocketLogicVectorXYAngle");
  InitSocket(cap_y, "Cap Up / Down", blender::SOCK_BOOLEAN, &cap_y_default);
  InitSocket(cap_y_range,
             "Cap Up / Down Range",
             blender::SOCK_VECTOR,
             &cap_y_range_default,
             "NodeSocketLogicVectorXYAngle");
  InitSocket(smoothing, "Smoothing", blender::SOCK_FLOAT, &smoothing_default);
  Append(mouse_look.inputs, flow);
  Append(mouse_look.inputs, body);
  Append(mouse_look.inputs, head);
  Append(mouse_look.inputs, inverted);
  Append(mouse_look.inputs, sensitivity);
  Append(mouse_look.inputs, cap_x);
  Append(mouse_look.inputs, cap_x_range);
  Append(mouse_look.inputs, cap_y);
  Append(mouse_look.inputs, cap_y_range);
  Append(mouse_look.inputs, smoothing);
  Append(tree.nodes, mouse_look);

  blender::bNodeLink flow_link = {};
  InitLink(flow_link, on_update, on_update_pulse, mouse_look, flow);
  Append(tree.links, flow_link);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  const LN_Instruction *instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::MouseLook);
  ASSERT_NE(instruction, nullptr);
  EXPECT_FALSE(instruction->bool_value);
  EXPECT_EQ(instruction->int_value, 0);
  EXPECT_EQ(program->GetDependencySummary().input_channels & LN_DEP_INPUT_MOUSE,
            uint32_t(LN_DEP_INPUT_MOUSE));
}

TEST(LN_TreeCompiler, RaycastCollisionLayerMaskDoesNotInvalidateParallelMouseLookBranch)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnFixedUpdate", "On Fixed Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueVector invert_default = {};
  invert_default.value[0] = 1.0f;
  invert_default.value[1] = 1.0f;
  blender::bNodeSocketValueFloat sensitivity_default = {};
  sensitivity_default.value = 1000.0f;
  blender::bNodeSocketValueBoolean cap_x_default = {};
  blender::bNodeSocketValueVector cap_x_range_default = {};
  blender::bNodeSocketValueBoolean cap_y_default = {};
  blender::bNodeSocketValueVector cap_y_range_default = {};
  blender::bNodeSocketValueFloat smoothing_default = {};

  blender::bNode mouse_look = {};
  blender::bNodeSocket mouse_flow = {};
  blender::bNodeSocket mouse_body = {};
  blender::bNodeSocket mouse_head = {};
  blender::bNodeSocket mouse_inverted = {};
  blender::bNodeSocket mouse_sensitivity = {};
  blender::bNodeSocket mouse_cap_x = {};
  blender::bNodeSocket mouse_cap_x_range = {};
  blender::bNodeSocket mouse_cap_y = {};
  blender::bNodeSocket mouse_cap_y_range = {};
  blender::bNodeSocket mouse_smoothing = {};
  InitNode(mouse_look, "LogicNativeMouseLook", "Mouse Look", 2);
  mouse_look.custom1 = 0;
  mouse_look.custom2 = 2 | 4;
  InitSocket(mouse_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(mouse_body, "Body", blender::SOCK_OBJECT, nullptr);
  InitSocket(mouse_head, "Head", blender::SOCK_OBJECT, nullptr);
  InitSocket(mouse_inverted, "Inverted", blender::SOCK_VECTOR, &invert_default);
  InitSocket(mouse_sensitivity, "Sensitivity", blender::SOCK_FLOAT, &sensitivity_default);
  InitSocket(mouse_cap_x, "Cap Left / Right", blender::SOCK_BOOLEAN, &cap_x_default);
  InitSocket(mouse_cap_x_range,
             "Cap Left / Right Range",
             blender::SOCK_VECTOR,
             &cap_x_range_default,
             "NodeSocketLogicVectorXYAngle");
  InitSocket(mouse_cap_y, "Cap Up / Down", blender::SOCK_BOOLEAN, &cap_y_default);
  InitSocket(mouse_cap_y_range,
             "Cap Up / Down Range",
             blender::SOCK_VECTOR,
             &cap_y_range_default,
             "NodeSocketLogicVectorXYAngle");
  InitSocket(mouse_smoothing, "Smoothing", blender::SOCK_FLOAT, &smoothing_default);
  Append(mouse_look.inputs, mouse_flow);
  Append(mouse_look.inputs, mouse_body);
  Append(mouse_look.inputs, mouse_head);
  Append(mouse_look.inputs, mouse_inverted);
  Append(mouse_look.inputs, mouse_sensitivity);
  Append(mouse_look.inputs, mouse_cap_x);
  Append(mouse_look.inputs, mouse_cap_x_range);
  Append(mouse_look.inputs, mouse_cap_y);
  Append(mouse_look.inputs, mouse_cap_y_range);
  Append(mouse_look.inputs, mouse_smoothing);
  Append(tree.nodes, mouse_look);

  blender::bNodeSocketValueVector origin_default = {};
  blender::bNodeSocketValueVector destination_default = {};
  destination_default.value[2] = -100.0f;
  blender::bNodeSocketValueVector direction_default = {};
  direction_default.value[2] = 1.0f;
  blender::bNodeSocketValueFloat max_distance_default = {};
  max_distance_default.value = 100.0f;
  blender::bNodeSocketValueBoolean local_default = {};
  blender::bNodeSocketValueString property_default = {};
  blender::bNodeSocketValueBoolean xray_default = {};
  blender::bNodeSocketValueInt mask_default = {};
  mask_default.value = 341;
  blender::bNodeSocketValueBoolean include_sensors_default = {};
  blender::bNodeSocketValueBoolean hit_back_faces_default = {};
  blender::bNodeSocketValueBoolean visualize_default = {};
  visualize_default.value = true;

  blender::bNode raycast = {};
  blender::bNodeSocket ray_flow = {};
  blender::bNodeSocket ray_caster = {};
  blender::bNodeSocket ray_ignore = {};
  blender::bNodeSocket ray_ignore_objects = {};
  blender::bNodeSocket ray_origin = {};
  blender::bNodeSocket ray_destination = {};
  blender::bNodeSocket ray_direction = {};
  blender::bNodeSocket ray_max_distance = {};
  blender::bNodeSocket ray_local = {};
  blender::bNodeSocket ray_property = {};
  blender::bNodeSocket ray_xray = {};
  blender::bNodeSocket ray_mask = {};
  blender::bNodeSocket ray_include_sensors = {};
  blender::bNodeSocket ray_hit_back_faces = {};
  blender::bNodeSocket ray_visualize = {};
  blender::bNodeSocket ray_done = {};
  blender::bNodeSocket ray_hit_object = {};
  InitNode(raycast, "LogicNativeRaycast", "Raycast", 3);
  InitSocket(ray_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(ray_caster, "Caster", blender::SOCK_OBJECT, nullptr);
  InitSocket(ray_ignore, "Ignore Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(
      ray_ignore_objects, "Ignore Objects", blender::SOCK_CUSTOM, nullptr, "NodeSocketLogicList");
  InitSocket(ray_origin, "Origin", blender::SOCK_VECTOR, &origin_default);
  InitSocket(ray_destination, "Destination", blender::SOCK_VECTOR, &destination_default);
  InitSocket(ray_direction, "Direction", blender::SOCK_VECTOR, &direction_default);
  InitSocket(ray_max_distance, "Max Distance", blender::SOCK_FLOAT, &max_distance_default);
  InitSocket(ray_local, "Local", blender::SOCK_BOOLEAN, &local_default);
  InitSocket(ray_property, "Property", blender::SOCK_STRING, &property_default);
  InitSocket(ray_xray, "X-Ray", blender::SOCK_BOOLEAN, &xray_default);
  /* Old saved files used this socket idname before the UI rename. */
  InitSocket(
      ray_mask, "Mask", blender::SOCK_INT, &mask_default, "NodeSocketLogicCollisionCollections");
  InitSocket(
      ray_include_sensors, "Include Sensors", blender::SOCK_BOOLEAN, &include_sensors_default);
  InitSocket(ray_hit_back_faces, "Hit Backfaces", blender::SOCK_BOOLEAN, &hit_back_faces_default);
  InitSocket(ray_visualize, "Visualize", blender::SOCK_BOOLEAN, &visualize_default);
  InitSocket(ray_done, "Done", blender::SOCK_BOOLEAN, nullptr);
  InitSocketWithIdentifier(
      ray_hit_object, "Picked Object", "Hit Object", blender::SOCK_OBJECT, nullptr);
  Append(raycast.inputs, ray_flow);
  Append(raycast.inputs, ray_caster);
  Append(raycast.inputs, ray_ignore);
  Append(raycast.inputs, ray_ignore_objects);
  Append(raycast.inputs, ray_origin);
  Append(raycast.inputs, ray_destination);
  Append(raycast.inputs, ray_direction);
  Append(raycast.inputs, ray_max_distance);
  Append(raycast.inputs, ray_local);
  Append(raycast.inputs, ray_property);
  Append(raycast.inputs, ray_xray);
  Append(raycast.inputs, ray_mask);
  Append(raycast.inputs, ray_include_sensors);
  Append(raycast.inputs, ray_hit_back_faces);
  Append(raycast.inputs, ray_visualize);
  Append(raycast.outputs, ray_done);
  Append(raycast.outputs, ray_hit_object);
  Append(tree.nodes, raycast);

  blender::bNode print_node = {};
  blender::bNodeSocket print_flow = {};
  blender::bNodeSocket print_value = {};
  InitNode(print_node, "LogicNativePrint", "Print", 4);
  InitSocket(print_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocketWithIdentifier(print_value, "Message", "Value", blender::SOCK_CUSTOM, nullptr);
  Append(print_node.inputs, print_flow);
  Append(print_node.inputs, print_value);
  Append(tree.nodes, print_node);

  blender::bNodeLink update_to_mouse = {};
  blender::bNodeLink update_to_ray = {};
  blender::bNodeLink ray_hit_to_print_value = {};
  InitLink(update_to_mouse, on_update, on_update_pulse, mouse_look, mouse_flow);
  InitLink(update_to_ray, on_update, on_update_pulse, raycast, ray_flow);
  InitLink(ray_hit_to_print_value, raycast, ray_hit_object, print_node, print_value);
  Append(tree.links, update_to_mouse);
  Append(tree.links, update_to_ray);
  Append(tree.links, ray_hit_to_print_value);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  const std::vector<LN_Instruction> &instructions = program->GetInstructions(
      LN_Event::OnFixedUpdate);
  EXPECT_NE(FindInstruction(instructions, LN_OpCode::MouseLook), nullptr);
  EXPECT_EQ(FindInstruction(instructions, LN_OpCode::Print), nullptr);

  bool found_query_evaluation = false;
  for (const LN_Instruction &instruction : instructions) {
    if (instruction.opcode != LN_OpCode::Nop) {
      continue;
    }
    found_query_evaluation |= BoolExpressionTreeContainsKind(
        *program, instruction.bool_guard_expr_index, LN_BoolExpressionKind::PhysicsQueryDone);
  }
  EXPECT_TRUE(found_query_evaluation);

  const auto &queries = program->GetQueryExpressions();
  ASSERT_EQ(queries.size(), 1u);
  EXPECT_EQ(queries[0].kind, LN_QueryExpressionKind::Raycast);
  EXPECT_NE(queries[0].ignore_list_expr_index, LN_INVALID_INDEX);

  const LN_IntExpression *mask_expression = IntExpressionAt(*program, queries[0].int_expr_index);
  ASSERT_NE(mask_expression, nullptr);
  EXPECT_EQ(mask_expression->kind, LN_IntExpressionKind::Constant);
  EXPECT_EQ(mask_expression->int_value, 341);
}

TEST(LN_TreeCompiler, RaycastFromMeshCompilesEveryInputAndOutputToOneDetailedQuery)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket update_out = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(update_out, "Out", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  Append(on_update.outputs, update_out);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueObject source_default = {};
  blender::bNodeSocketValueObject ignore_default = {};
  blender::bNodeSocketValueFloat start_offset_default = {};
  start_offset_default.value = 0.125f;
  blender::bNodeSocketValueFloat max_distance_default = {};
  max_distance_default.value = 37.5f;
  blender::bNodeSocketValueInt max_rays_default = {};
  max_rays_default.value = 123;
  blender::bNodeSocketValueString property_default = {};
  SetCString(property_default.value, "mesh_hit");
  blender::bNodeSocketValueBoolean xray_default = {};
  xray_default.value = true;
  blender::bNodeSocketValueInt mask_default = {};
  mask_default.value = 341;
  blender::bNodeSocketValueBoolean include_sensors_default = {};
  include_sensors_default.value = true;
  blender::bNodeSocketValueBoolean hit_backfaces_default = {};
  hit_backfaces_default.value = true;
  blender::bNodeSocketValueBoolean visualize_default = {};
  visualize_default.value = true;

  blender::bNode raycast = {};
  raycast.custom1 = 1;
  raycast.custom2 = 1;
  InitNode(raycast, "LogicNativeRaycastFromMesh", "Raycast from Mesh", 2);
  std::array<blender::bNodeSocket, 13> inputs{};
  InitSocket(inputs[0], "Flow", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  InitSocket(inputs[1], "Source Object", blender::SOCK_OBJECT, &source_default);
  InitSocket(inputs[2], "Ignore Object", blender::SOCK_OBJECT, &ignore_default);
  InitSocket(inputs[3], "Ignore Objects", blender::SOCK_CUSTOM, nullptr, "NodeSocketLogicList");
  InitSocket(inputs[4], "Start Offset", blender::SOCK_FLOAT, &start_offset_default);
  InitSocket(inputs[5], "Max Distance", blender::SOCK_FLOAT, &max_distance_default);
  InitSocket(inputs[6], "Max Rays", blender::SOCK_INT, &max_rays_default);
  InitSocket(inputs[7], "Property", blender::SOCK_STRING, &property_default);
  InitSocket(inputs[8], "X-Ray", blender::SOCK_BOOLEAN, &xray_default);
  InitSocket(
      inputs[9], "Mask", blender::SOCK_INT, &mask_default, "NodeSocketLogicCollisionLayers");
  InitSocket(inputs[10], "Include Sensors", blender::SOCK_BOOLEAN, &include_sensors_default);
  InitSocket(inputs[11], "Hit Backfaces", blender::SOCK_BOOLEAN, &hit_backfaces_default);
  InitSocket(inputs[12], "Visualize", blender::SOCK_BOOLEAN, &visualize_default);
  for (blender::bNodeSocket &input : inputs) {
    Append(raycast.inputs, input);
  }

  constexpr std::array<const char *, 18> output_names = {
      "Done",          "Any Hit",        "All Hit",       "Hit Count",
      "Ray Count",     "Hits",           "Blocked",       "Hit Objects",
      "Points",        "Normals",        "Distances",     "Fractions",
      "Face Indices",  "Has UVs",        "UVs",           "Ray Origins",
      "Ray Directions", "End Points",
  };
  std::array<blender::bNodeSocket, output_names.size()> outputs{};
  for (size_t index = 0; index < outputs.size(); index++) {
    if (index == 0) {
      InitSocket(outputs[index],
                 output_names[index],
                 blender::SOCK_BOOLEAN,
                 nullptr,
                 "NodeSocketLogicExecution");
    }
    else if (index <= 2) {
      InitSocket(outputs[index],
                 output_names[index],
                 blender::SOCK_BOOLEAN,
                 nullptr,
                 "NodeSocketLogicCondition");
    }
    else if (index <= 4) {
      InitSocket(outputs[index], output_names[index], blender::SOCK_INT, nullptr);
    }
    else {
      InitSocket(outputs[index],
                 output_names[index],
                 blender::SOCK_CUSTOM,
                 nullptr,
                 "NodeSocketLogicList");
    }
    Append(raycast.outputs, outputs[index]);
  }
  Append(tree.nodes, raycast);

  constexpr size_t value_output_count = output_names.size() - 1;
  std::array<blender::bNode, value_output_count> print_nodes{};
  std::array<blender::bNodeSocket, value_output_count> print_flows{};
  std::array<blender::bNodeSocket, value_output_count> print_values{};
  std::array<blender::bNodeLink, 1 + value_output_count * 2> links{};
  InitLink(links[0], on_update, update_out, raycast, inputs[0]);
  Append(tree.links, links[0]);
  for (size_t index = 0; index < value_output_count; index++) {
    InitNode(print_nodes[index], "LogicNativePrint", "Print Mesh Ray Output", int32_t(3 + index));
    InitSocket(print_flows[index],
               "Flow",
               blender::SOCK_BOOLEAN,
               nullptr,
               "NodeSocketLogicExecution");
    InitSocketWithIdentifier(
        print_values[index], "Message", "Value", blender::SOCK_CUSTOM, nullptr);
    Append(print_nodes[index].inputs, print_flows[index]);
    Append(print_nodes[index].inputs, print_values[index]);
    Append(tree.nodes, print_nodes[index]);

    InitLink(links[1 + index * 2],
             raycast,
             outputs[0],
             print_nodes[index],
             print_flows[index]);
    InitLink(links[2 + index * 2],
             raycast,
             outputs[index + 1],
             print_nodes[index],
             print_values[index]);
    Append(tree.links, links[1 + index * 2]);
    Append(tree.links, links[2 + index * 2]);
  }

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(program, nullptr);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  ASSERT_FALSE(program->GetQueryExpressions().empty());
  const LN_QueryExpression &query = program->GetQueryExpressions().front();
  EXPECT_EQ(query.kind, LN_QueryExpressionKind::RaycastFromMesh);
  EXPECT_EQ(query.mesh_ray_domain, LN_MeshRayDomain::Faces);
  EXPECT_EQ(query.mesh_ray_direction, LN_MeshRayDirection::InvertedNormals);
  EXPECT_EQ(query.ray_query_detail_flags,
            LN_RAY_QUERY_DETAIL_FACE_INDEX | LN_RAY_QUERY_DETAIL_UV);
  EXPECT_FLOAT_EQ(query.float_value, 0.125f);
  EXPECT_FLOAT_EQ(query.secondary_float_value, 37.5f);
  EXPECT_EQ(query.int_value, 341);
  EXPECT_EQ(query.secondary_int_value, 123);
  EXPECT_NE(query.input1, LN_INVALID_INDEX);
  EXPECT_NE(query.ignore_list_expr_index, LN_INVALID_INDEX);
  EXPECT_NE(query.condition_bool_expr_index, LN_INVALID_INDEX);
  EXPECT_NE(query.string_expr_index, LN_INVALID_INDEX);
  EXPECT_NE(query.secondary_bool_expr_index, LN_INVALID_INDEX);
  EXPECT_NE(query.tertiary_bool_expr_index, LN_INVALID_INDEX);
  EXPECT_NE(query.quaternary_bool_expr_index, LN_INVALID_INDEX);
  EXPECT_NE(query.quinary_bool_expr_index, LN_INVALID_INDEX);
  for (const LN_QueryExpression &shared_query : program->GetQueryExpressions()) {
    EXPECT_EQ(shared_query.kind, LN_QueryExpressionKind::RaycastFromMesh);
    EXPECT_EQ(shared_query.runtime_state_index, query.runtime_state_index);
    EXPECT_EQ(shared_query.mesh_ray_domain, query.mesh_ray_domain);
    EXPECT_EQ(shared_query.mesh_ray_direction, query.mesh_ray_direction);
    EXPECT_EQ(shared_query.ray_query_detail_flags, query.ray_query_detail_flags);
  }
  EXPECT_EQ(program->GetSchedulerSummary().purity, LN_SchedulerPurity::MainThreadOnly);

  const auto has_bool_kind = [&](const LN_BoolExpressionKind kind) {
    return std::any_of(program->GetBoolExpressions().begin(),
                       program->GetBoolExpressions().end(),
                       [&](const LN_BoolExpression &expression) { return expression.kind == kind; });
  };
  const auto has_int_kind = [&](const LN_IntExpressionKind kind) {
    return std::any_of(program->GetIntExpressions().begin(),
                       program->GetIntExpressions().end(),
                       [&](const LN_IntExpression &expression) { return expression.kind == kind; });
  };
  const auto has_value_kind = [&](const LN_ValueExpressionKind kind) {
    return std::any_of(program->GetValueExpressions().begin(),
                       program->GetValueExpressions().end(),
                       [&](const LN_ValueExpression &expression) { return expression.kind == kind; });
  };
  EXPECT_TRUE(has_bool_kind(LN_BoolExpressionKind::PhysicsQueryDone));
  EXPECT_TRUE(has_bool_kind(LN_BoolExpressionKind::PhysicsQueryHit));
  EXPECT_TRUE(has_bool_kind(LN_BoolExpressionKind::PhysicsQueryAllHit));
  EXPECT_TRUE(has_int_kind(LN_IntExpressionKind::PhysicsQueryHitCount));
  EXPECT_TRUE(has_int_kind(LN_IntExpressionKind::PhysicsQueryRayCount));
  for (const LN_ValueExpressionKind kind : {
           LN_ValueExpressionKind::PhysicsQueryHits,
           LN_ValueExpressionKind::PhysicsQueryBlockedList,
           LN_ValueExpressionKind::PhysicsQueryObjects,
           LN_ValueExpressionKind::PhysicsQueryPoints,
           LN_ValueExpressionKind::PhysicsQueryNormals,
           LN_ValueExpressionKind::PhysicsQueryDistances,
           LN_ValueExpressionKind::PhysicsQueryFractions,
           LN_ValueExpressionKind::PhysicsQueryFaceIndices,
           LN_ValueExpressionKind::PhysicsQueryHasUVs,
           LN_ValueExpressionKind::PhysicsQueryUVs,
           LN_ValueExpressionKind::PhysicsQueryRayOrigins,
           LN_ValueExpressionKind::PhysicsQueryRayDirections,
           LN_ValueExpressionKind::PhysicsQueryEndPoints,
       })
  {
    EXPECT_TRUE(has_value_kind(kind)) << int(kind);
  }
}

TEST(LN_TreeCompiler, EmitsRuntimeBranchGuardFromGamePropertyBool)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnFixedUpdate", "On Fixed Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueString property_name_default = {};
  SetCString(property_name_default.value, "enabled");
  blender::bNode get_property = {};
  blender::bNodeSocket property_name = {};
  blender::bNodeSocket property_value = {};
  blender::bNodeSocket property_object = {};
  InitNode(get_property, "LogicNativeGetGamePropertyBool", "Get Enabled", 2);
  InitSocket(property_object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(property_name, "Property", blender::SOCK_STRING, &property_name_default);
  InitSocket(property_value, "Value", blender::SOCK_BOOLEAN, nullptr);
  Append(get_property.inputs, property_object);
  Append(get_property.inputs, property_name);
  Append(get_property.outputs, property_value);
  Append(tree.nodes, get_property);

  blender::bNodeSocketValueBoolean condition_default = {};
  blender::bNode branch = {};
  blender::bNodeSocket branch_flow = {};
  blender::bNodeSocket condition = {};
  blender::bNodeSocket true_output = {};
  blender::bNodeSocket false_output = {};
  InitNode(branch, "LogicNativeBranch", "Branch", 3);
  InitSocket(branch_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(condition, "Condition", blender::SOCK_BOOLEAN, &condition_default);
  InitSocket(true_output, "True", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(false_output, "False", blender::SOCK_BOOLEAN, nullptr);
  Append(branch.inputs, branch_flow);
  Append(branch.inputs, condition);
  Append(branch.outputs, true_output);
  Append(branch.outputs, false_output);
  Append(tree.nodes, branch);

  blender::bNodeSocketValueVector position_default = {};
  position_default.value[0] = 1.0f;
  blender::bNode set_position = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket position = {};
  SetObjectAttributeAuxSockets set_attribute_aux = {};
  InitSetObjectAttributeVectorNode(
      set_position, set_attribute_aux, flow, position, "Set Position", 4, 0, &position_default);
  Append(tree.nodes, set_position);

  blender::bNodeLink event_to_branch = {};
  blender::bNodeLink property_to_condition = {};
  blender::bNodeLink branch_to_set_position = {};
  InitLink(event_to_branch, on_update, on_update_pulse, branch, branch_flow);
  InitLink(property_to_condition, get_property, property_value, branch, condition);
  InitLink(branch_to_set_position, branch, true_output, set_position, flow);
  Append(tree.links, event_to_branch);
  Append(tree.links, property_to_condition);
  Append(tree.links, branch_to_set_position);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors());
  EXPECT_TRUE(HasGamePropertyRef(*program, "enabled", LN_ValueType::Bool));

  const std::vector<LN_Instruction> &instructions = program->GetInstructions(
      LN_Event::OnFixedUpdate);
  const int route_index = FindInstructionIndex(instructions, LN_OpCode::BranchRoute);
  const int set_index = FindInstructionIndex(instructions, LN_OpCode::SetTransformVector);
  ASSERT_GE(route_index, 0);
  ASSERT_GE(set_index, 0);
  EXPECT_LT(route_index, set_index);

  const LN_Instruction &route_instruction = instructions[size_t(route_index)];
  const LN_BoolExpression *route_condition = BoolExpressionAt(*program,
                                                              route_instruction.bool_expr_index);
  ASSERT_NE(route_condition, nullptr);
  EXPECT_EQ(route_condition->kind, LN_BoolExpressionKind::SnapshotGameProperty);

  const LN_Instruction &set_instruction = instructions[size_t(set_index)];
  ASSERT_NE(set_instruction.bool_guard_expr_index, LN_INVALID_INDEX);
  const LN_BoolExpression *guard = BoolExpressionAt(*program,
                                                    set_instruction.bool_guard_expr_index);
  ASSERT_NE(guard, nullptr);
  EXPECT_EQ(guard->kind, LN_BoolExpressionKind::InstructionExecuted);
  EXPECT_EQ(guard->input0, uint32_t(route_index));
}

TEST(LN_TreeCompiler, GateResultFeedsExplicitBranchRouteCondition)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnFixedUpdate", "On Fixed Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueString property_a_default = {};
  SetCString(property_a_default.value, "enabled_a");
  blender::bNode get_property_a = {};
  blender::bNodeSocket property_a_object = {};
  blender::bNodeSocket property_a_name = {};
  blender::bNodeSocket property_a_value = {};
  InitNode(get_property_a, "LogicNativeGetGamePropertyBool", "Get Enabled A", 2);
  InitSocket(property_a_object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(property_a_name, "Property", blender::SOCK_STRING, &property_a_default);
  InitSocket(property_a_value, "Value", blender::SOCK_BOOLEAN, nullptr);
  Append(get_property_a.inputs, property_a_object);
  Append(get_property_a.inputs, property_a_name);
  Append(get_property_a.outputs, property_a_value);
  Append(tree.nodes, get_property_a);

  blender::bNodeSocketValueString property_b_default = {};
  SetCString(property_b_default.value, "enabled_b");
  blender::bNode get_property_b = {};
  blender::bNodeSocket property_b_object = {};
  blender::bNodeSocket property_b_name = {};
  blender::bNodeSocket property_b_value = {};
  InitNode(get_property_b, "LogicNativeGetGamePropertyBool", "Get Enabled B", 3);
  InitSocket(property_b_object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(property_b_name, "Property", blender::SOCK_STRING, &property_b_default);
  InitSocket(property_b_value, "Value", blender::SOCK_BOOLEAN, nullptr);
  Append(get_property_b.inputs, property_b_object);
  Append(get_property_b.inputs, property_b_name);
  Append(get_property_b.outputs, property_b_value);
  Append(tree.nodes, get_property_b);

  blender::bNode gate = {};
  blender::bNodeSocket gate_conditions = {};
  blender::bNodeSocket gate_result = {};
  InitNode(gate, "LogicNativeGate", "Gate", 4);
  gate.custom1 = 1;
  InitSocket(gate_conditions, "Conditions", blender::SOCK_BOOLEAN, nullptr);
  gate_conditions.flag |= blender::SOCK_MULTI_INPUT;
  InitSocket(gate_result, "Result", blender::SOCK_BOOLEAN, nullptr);
  Append(gate.inputs, gate_conditions);
  Append(gate.outputs, gate_result);
  Append(tree.nodes, gate);

  blender::bNodeSocketValueBoolean condition_default = {};
  blender::bNode branch = {};
  blender::bNodeSocket branch_flow = {};
  blender::bNodeSocket branch_condition = {};
  blender::bNodeSocket true_output = {};
  blender::bNodeSocket false_output = {};
  InitNode(branch, "LogicNativeBranch", "Branch", 5);
  InitSocket(branch_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(branch_condition, "Condition", blender::SOCK_BOOLEAN, &condition_default);
  InitSocket(true_output, "True", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(false_output, "False", blender::SOCK_BOOLEAN, nullptr);
  Append(branch.inputs, branch_flow);
  Append(branch.inputs, branch_condition);
  Append(branch.outputs, true_output);
  Append(branch.outputs, false_output);
  Append(tree.nodes, branch);

  blender::bNodeSocketValueVector position_default = {};
  position_default.value[0] = 7.0f;
  blender::bNode set_position = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket position = {};
  SetObjectAttributeAuxSockets set_attribute_aux = {};
  InitSetObjectAttributeVectorNode(
      set_position, set_attribute_aux, flow, position, "Set Position", 6, 0, &position_default);
  Append(tree.nodes, set_position);

  blender::bNodeLink event_to_branch = {};
  blender::bNodeLink property_a_to_gate = {};
  blender::bNodeLink property_b_to_gate = {};
  blender::bNodeLink gate_to_branch = {};
  blender::bNodeLink branch_to_set_position = {};
  InitLink(event_to_branch, on_update, on_update_pulse, branch, branch_flow);
  InitLink(property_a_to_gate, get_property_a, property_a_value, gate, gate_conditions);
  InitLink(property_b_to_gate, get_property_b, property_b_value, gate, gate_conditions);
  property_a_to_gate.multi_input_sort_id = 1;
  property_b_to_gate.multi_input_sort_id = 0;
  InitLink(gate_to_branch, gate, gate_result, branch, branch_condition);
  InitLink(branch_to_set_position, branch, true_output, set_position, flow);
  Append(tree.links, event_to_branch);
  Append(tree.links, property_a_to_gate);
  Append(tree.links, property_b_to_gate);
  Append(tree.links, gate_to_branch);
  Append(tree.links, branch_to_set_position);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());

  const std::vector<LN_Instruction> &instructions = program->GetInstructions(
      LN_Event::OnFixedUpdate);
  const int route_index = FindInstructionIndex(instructions, LN_OpCode::BranchRoute);
  const int set_index = FindInstructionIndex(instructions, LN_OpCode::SetTransformVector);
  ASSERT_GE(route_index, 0);
  ASSERT_GE(set_index, 0);

  const LN_Instruction &route_instruction = instructions[size_t(route_index)];
  const LN_BoolExpression *route_condition = BoolExpressionAt(*program,
                                                              route_instruction.bool_expr_index);
  ASSERT_NE(route_condition, nullptr);
  EXPECT_EQ(route_condition->kind, LN_BoolExpressionKind::Or);
  EXPECT_TRUE(BoolExpressionTreeContainsKind(
      *program, route_instruction.bool_expr_index, LN_BoolExpressionKind::SnapshotGameProperty));

  const LN_Instruction &set_instruction = instructions[size_t(set_index)];
  const LN_BoolExpression *guard = BoolExpressionAt(*program,
                                                    set_instruction.bool_guard_expr_index);
  ASSERT_NE(guard, nullptr);
  EXPECT_EQ(guard->kind, LN_BoolExpressionKind::InstructionExecuted);
  EXPECT_EQ(guard->input0, uint32_t(route_index));
}

TEST(LN_TreeCompiler, BooleanCombineSupportsFiveInputsAndRejectsMultiInputNot)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket update_out = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(update_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, update_out);
  Append(tree.nodes, on_update);

  std::array<blender::bNodeSocketValueBoolean, 5> value_defaults = {};
  std::array<blender::bNode, 5> values = {};
  std::array<blender::bNodeSocket, 5> value_outputs = {};
  for (int index = 0; index < int(values.size()); index++) {
    /* NAND(false, false, true, true, true) is true. Pairwise NAND reduction is incorrectly false.
     */
    value_defaults[size_t(index)].value = index >= 2;
    const std::string name = "Condition " + std::to_string(index);
    InitNode(values[size_t(index)], "LogicNativeValueBool", name.c_str(), int32_t(index + 2));
    InitSocket(value_outputs[size_t(index)],
               "Bool",
               blender::SOCK_BOOLEAN,
               &value_defaults[size_t(index)]);
    Append(values[size_t(index)].outputs, value_outputs[size_t(index)]);
    Append(tree.nodes, values[size_t(index)]);
  }

  blender::bNode combine = {};
  blender::bNodeSocket conditions = {};
  blender::bNodeSocket result = {};
  InitNode(combine, "LogicNativeGate", "Boolean Combine", 7);
  combine.custom1 = 4;
  InitSocket(conditions, "Conditions", blender::SOCK_BOOLEAN, nullptr);
  conditions.flag |= blender::SOCK_MULTI_INPUT;
  InitSocket(result, "Result", blender::SOCK_BOOLEAN, nullptr);
  Append(combine.inputs, conditions);
  Append(combine.outputs, result);
  Append(tree.nodes, combine);

  blender::bNodeSocketValueBoolean branch_condition_default = {};
  blender::bNode branch = {};
  blender::bNodeSocket branch_flow = {};
  blender::bNodeSocket branch_condition = {};
  blender::bNodeSocket branch_true = {};
  blender::bNodeSocket branch_false = {};
  InitNode(branch, "LogicNativeBranch", "Branch", 8);
  InitSocket(branch_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(branch_condition, "Condition", blender::SOCK_BOOLEAN, &branch_condition_default);
  InitSocket(branch_true, "True", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(branch_false, "False", blender::SOCK_BOOLEAN, nullptr);
  Append(branch.inputs, branch_flow);
  Append(branch.inputs, branch_condition);
  Append(branch.outputs, branch_true);
  Append(branch.outputs, branch_false);
  Append(tree.nodes, branch);

  blender::bNode quit = {};
  blender::bNodeSocket quit_flow = {};
  InitNode(quit, "LogicNativeQuitGame", "Quit Game", 9);
  InitSocket(quit_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  Append(quit.inputs, quit_flow);
  Append(tree.nodes, quit);

  blender::bNodeLink event_to_branch = {};
  std::array<blender::bNodeLink, 5> value_to_combine = {};
  blender::bNodeLink combine_to_branch = {};
  blender::bNodeLink branch_to_quit = {};
  InitLink(event_to_branch, on_update, update_out, branch, branch_flow);
  Append(tree.links, event_to_branch);
  for (int index = 0; index < int(values.size()); index++) {
    InitLink(value_to_combine[size_t(index)],
             values[size_t(index)],
             value_outputs[size_t(index)],
             combine,
             conditions);
    value_to_combine[size_t(index)].multi_input_sort_id = int(values.size()) - index - 1;
    Append(tree.links, value_to_combine[size_t(index)]);
  }
  InitLink(combine_to_branch, combine, result, branch, branch_condition);
  InitLink(branch_to_quit, branch, branch_true, quit, quit_flow);
  Append(tree.links, combine_to_branch);
  Append(tree.links, branch_to_quit);

  const std::shared_ptr<LN_Program> nand_program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(nand_program->GetCompileReport().HasErrors())
      << FormatCompileReport(nand_program->GetCompileReport());
  const LN_Instruction *route = nullptr;
  for (const LN_Instruction &instruction : nand_program->GetInstructions(LN_Event::OnUpdate)) {
    if (instruction.opcode != LN_OpCode::BranchRoute ||
        instruction.source_ref_index >= nand_program->GetSourceRefs().size())
    {
      continue;
    }
    if (nand_program->GetSourceRefs()[instruction.source_ref_index].socket_name == "True") {
      route = &instruction;
      break;
    }
  }
  ASSERT_NE(route, nullptr);
  const LN_BoolExpression *nand = BoolExpressionAt(*nand_program, route->bool_expr_index);
  ASSERT_NE(nand, nullptr);
  ASSERT_EQ(nand->kind, LN_BoolExpressionKind::Constant);
  EXPECT_TRUE(nand->bool_value);

  combine.custom1 = 3;
  const std::shared_ptr<LN_Program> invalid_not_program = LN_TreeCompiler().Compile(tree);
  EXPECT_TRUE(invalid_not_program->GetCompileReport().HasErrors());
  EXPECT_TRUE(HasErrorContaining(invalid_not_program->GetCompileReport(),
                                 "Not accepts exactly one condition"))
      << FormatCompileReport(invalid_not_program->GetCompileReport());
}

TEST(LN_TreeCompiler, EmitsSetGamePropertyFloatInstruction)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_init = {};
  blender::bNodeSocket on_init_pulse = {};
  InitNode(on_init, "LogicNativeOnInit", "On Init", 1);
  InitSocket(on_init_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_init.outputs, on_init_pulse);
  Append(tree.nodes, on_init);

  blender::bNodeSocketValueString property_name_default = {};
  SetCString(property_name_default.value, "speed");
  blender::bNodeSocketValueFloat value_default = {};
  value_default.value = 8.0f;
  blender::bNode set_property = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket property_name = {};
  blender::bNodeSocket value = {};
  blender::bNodeSocket property_object = {};
  InitNode(set_property, "LogicNativeSetGamePropertyFloat", "Set Speed", 2);
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(property_object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(property_name, "Property", blender::SOCK_STRING, &property_name_default);
  InitSocket(value, "Value", blender::SOCK_FLOAT, &value_default);
  Append(set_property.inputs, flow);
  Append(set_property.inputs, property_object);
  Append(set_property.inputs, property_name);
  Append(set_property.inputs, value);
  Append(tree.nodes, set_property);

  blender::bNodeLink flow_link = {};
  InitLink(flow_link, on_init, on_init_pulse, set_property, flow);
  Append(tree.links, flow_link);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors());
  const LN_Instruction *instruction = FindInstruction(program->GetInstructions(LN_Event::OnInit),
                                                      LN_OpCode::SetGameProperty);
  ASSERT_NE(instruction, nullptr);
  EXPECT_EQ(instruction->property_value_type, LN_ValueType::Float);
  const LN_GamePropertyRef *property_ref = GamePropertyRefAt(*program,
                                                             instruction->property_ref_index);
  ASSERT_NE(property_ref, nullptr);
  EXPECT_EQ(property_ref->name, "speed");
  EXPECT_NE(instruction->float_expr_index, LN_INVALID_INDEX);
}

TEST(LN_TreeCompiler, ModifyPropertySupportsOperationClampAndTargetObject)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_init = {};
  blender::bNodeSocket on_init_pulse = {};
  InitNode(on_init, "LogicNativeOnInit", "On Init", 1);
  InitSocket(on_init_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_init.outputs, on_init_pulse);
  Append(tree.nodes, on_init);

  blender::bNode owner = {};
  blender::bNodeSocket owner_output = {};
  InitNode(owner, "LogicNativeGetOwner", "Owner", 2);
  InitSocket(owner_output, "Owner Object", blender::SOCK_OBJECT, nullptr);
  Append(owner.outputs, owner_output);
  Append(tree.nodes, owner);

  blender::bNodeSocketValueString property_name_default = {};
  SetCString(property_name_default.value, "speed");
  blender::bNodeSocketValueFloat value_default = {};
  value_default.value = 2.0f;
  blender::bNodeSocketValueFloat min_default = {};
  min_default.value = 0.0f;
  blender::bNodeSocketValueFloat max_default = {};
  max_default.value = 10.0f;
  blender::bNode modify_property = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket object = {};
  blender::bNodeSocket property_name = {};
  blender::bNodeSocket value = {};
  blender::bNodeSocket min = {};
  blender::bNodeSocket max = {};
  InitNode(modify_property, "LogicNativeModifyProperty", "Modify Speed", 3);
  modify_property.custom1 = blender::NODE_MATH_MULTIPLY;
  modify_property.custom2 = 1;
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(property_name, "Property", blender::SOCK_STRING, &property_name_default);
  InitSocket(value, "Value", blender::SOCK_FLOAT, &value_default);
  InitSocket(min, "Min", blender::SOCK_FLOAT, &min_default);
  InitSocket(max, "Max", blender::SOCK_FLOAT, &max_default);
  Append(modify_property.inputs, flow);
  Append(modify_property.inputs, object);
  Append(modify_property.inputs, property_name);
  Append(modify_property.inputs, value);
  Append(modify_property.inputs, min);
  Append(modify_property.inputs, max);
  Append(tree.nodes, modify_property);

  blender::bNodeLink flow_link = {};
  blender::bNodeLink object_link = {};
  InitLink(flow_link, on_init, on_init_pulse, modify_property, flow);
  InitLink(object_link, owner, owner_output, modify_property, object);
  Append(tree.links, flow_link);
  Append(tree.links, object_link);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  const LN_Instruction *instruction = FindInstruction(program->GetInstructions(LN_Event::OnInit),
                                                      LN_OpCode::SetGameProperty);
  ASSERT_NE(instruction, nullptr);
  EXPECT_NE(instruction->value_expr_index, LN_INVALID_INDEX);
  EXPECT_EQ(instruction->property_value_type, LN_ValueType::Float);
  EXPECT_TRUE(HasGamePropertyRef(*program, "speed", LN_ValueType::Float));

  /* The operation and clamp are applied to the live property value when the command flushes. */
  EXPECT_EQ(LN_PropertyModifyOp(instruction->int_value), LN_PropertyModifyOp::Multiply);
  const LN_FloatExpression *delta = FloatExpressionAt(*program, instruction->float_expr_index);
  ASSERT_NE(delta, nullptr);
  EXPECT_EQ(delta->kind, LN_FloatExpressionKind::Constant);
  EXPECT_FLOAT_EQ(delta->float_value, 2.0f);
  const LN_FloatExpression *min_value = FloatExpressionAt(*program,
                                                          instruction->secondary_float_expr_index);
  const LN_FloatExpression *max_value = FloatExpressionAt(*program,
                                                          instruction->tertiary_float_expr_index);
  ASSERT_NE(min_value, nullptr);
  ASSERT_NE(max_value, nullptr);
  EXPECT_FLOAT_EQ(min_value->float_value, 0.0f);
  EXPECT_FLOAT_EQ(max_value->float_value, 10.0f);
}

TEST(LN_TreeCompiler, ModifyPropertyFloorDivideBuildsFloorOfDivide)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_init = {};
  blender::bNodeSocket on_init_pulse = {};
  InitNode(on_init, "LogicNativeOnInit", "On Init", 1);
  InitSocket(on_init_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_init.outputs, on_init_pulse);
  Append(tree.nodes, on_init);

  blender::bNodeSocketValueString property_name_default = {};
  SetCString(property_name_default.value, "ammo");
  blender::bNodeSocketValueFloat value_default = {};
  value_default.value = 3.0f;
  blender::bNodeSocketValueFloat min_default = {};
  blender::bNodeSocketValueFloat max_default = {};
  max_default.value = 1.0f;
  blender::bNode modify_property = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket object = {};
  blender::bNodeSocket property_name = {};
  blender::bNodeSocket value = {};
  blender::bNodeSocket min = {};
  blender::bNodeSocket max = {};
  InitNode(modify_property, "LogicNativeModifyProperty", "Modify Ammo", 2);
  modify_property.custom1 = blender::NODE_MATH_FLOOR;
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(property_name, "Property", blender::SOCK_STRING, &property_name_default);
  InitSocket(value, "Value", blender::SOCK_FLOAT, &value_default);
  InitSocket(min, "Min", blender::SOCK_FLOAT, &min_default);
  InitSocket(max, "Max", blender::SOCK_FLOAT, &max_default);
  Append(modify_property.inputs, flow);
  Append(modify_property.inputs, object);
  Append(modify_property.inputs, property_name);
  Append(modify_property.inputs, value);
  Append(modify_property.inputs, min);
  Append(modify_property.inputs, max);
  Append(tree.nodes, modify_property);

  blender::bNodeLink flow_link = {};
  InitLink(flow_link, on_init, on_init_pulse, modify_property, flow);
  Append(tree.links, flow_link);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  const LN_Instruction *instruction = FindInstruction(program->GetInstructions(LN_Event::OnInit),
                                                      LN_OpCode::SetGameProperty);
  ASSERT_NE(instruction, nullptr);

  EXPECT_EQ(LN_PropertyModifyOp(instruction->int_value), LN_PropertyModifyOp::FloorDivide);
  const LN_FloatExpression *delta = FloatExpressionAt(*program, instruction->float_expr_index);
  ASSERT_NE(delta, nullptr);
  EXPECT_EQ(delta->kind, LN_FloatExpressionKind::Constant);
  EXPECT_FLOAT_EQ(delta->float_value, 3.0f);
}

TEST(LN_TreeCompiler, ModifyPropertyIgnoresRemovedAttributeModeBit)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_init = {};
  blender::bNodeSocket on_init_pulse = {};
  InitNode(on_init, "LogicNativeOnInit", "On Init", 1);
  InitSocket(on_init_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_init.outputs, on_init_pulse);
  Append(tree.nodes, on_init);

  blender::bNodeSocketValueString property_name_default = {};
  SetCString(property_name_default.value, "location");
  blender::bNodeSocketValueFloat value_default = {};
  value_default.value = 1.0f;
  blender::bNodeSocketValueFloat min_default = {};
  blender::bNodeSocketValueFloat max_default = {};
  max_default.value = 1.0f;
  blender::bNode modify_property = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket object = {};
  blender::bNodeSocket property_name = {};
  blender::bNodeSocket value = {};
  blender::bNodeSocket min = {};
  blender::bNodeSocket max = {};
  InitNode(modify_property, "LogicNativeModifyProperty", "Modify Attribute", 2);
  modify_property.custom1 = blender::NODE_MATH_ADD;
  modify_property.custom2 = 2;
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(property_name, "Property", blender::SOCK_STRING, &property_name_default);
  InitSocket(value, "Value", blender::SOCK_FLOAT, &value_default);
  InitSocket(min, "Min", blender::SOCK_FLOAT, &min_default);
  InitSocket(max, "Max", blender::SOCK_FLOAT, &max_default);
  Append(modify_property.inputs, flow);
  Append(modify_property.inputs, object);
  Append(modify_property.inputs, property_name);
  Append(modify_property.inputs, value);
  Append(modify_property.inputs, min);
  Append(modify_property.inputs, max);
  Append(tree.nodes, modify_property);

  blender::bNodeLink flow_link = {};
  InitLink(flow_link, on_init, on_init_pulse, modify_property, flow);
  Append(tree.links, flow_link);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  const LN_Instruction *instruction = FindInstruction(program->GetInstructions(LN_Event::OnInit),
                                                      LN_OpCode::SetGameProperty);
  ASSERT_NE(instruction, nullptr);
  EXPECT_EQ(LN_PropertyModifyOp(instruction->int_value), LN_PropertyModifyOp::Add);
}

TEST(LN_TreeCompiler, EmitsVectorScaleFromGamePropertyFloat)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnFixedUpdate", "On Fixed Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueString property_name_default = {};
  SetCString(property_name_default.value, "scale");
  blender::bNode get_property = {};
  blender::bNodeSocket property_name = {};
  blender::bNodeSocket property_value = {};
  blender::bNodeSocket property_object = {};
  InitNode(get_property, "LogicNativeGetGamePropertyFloat", "Get Scale", 2);
  InitSocket(property_object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(property_name, "Property", blender::SOCK_STRING, &property_name_default);
  InitSocket(property_value, "Value", blender::SOCK_FLOAT, nullptr);
  Append(get_property.inputs, property_object);
  Append(get_property.inputs, property_name);
  Append(get_property.outputs, property_value);
  Append(tree.nodes, get_property);

  blender::bNodeSocketValueVector vector_default = {};
  vector_default.value[0] = 1.0f;
  blender::bNode vector_value = {};
  blender::bNodeSocket vector_value_output = {};
  InitNode(vector_value, "LogicNativeValueVector", "Vector", 3);
  InitSocket(vector_value_output, "Vector", blender::SOCK_VECTOR, &vector_default);
  Append(vector_value.outputs, vector_value_output);
  Append(tree.nodes, vector_value);

  blender::bNodeSocketValueVector math_a_default = {};
  blender::bNodeSocketValueVector math_b_default = {};
  blender::bNodeSocketValueFloat scale_default = {};
  blender::bNode vector_math = {};
  blender::bNodeSocket math_a = {};
  blender::bNodeSocket math_b = {};
  blender::bNodeSocket math_scale = {};
  blender::bNodeSocket math_result = {};
  InitNode(vector_math, "LogicNativeVectorMath", "Vector Math", 4);
  vector_math.custom1 = blender::NODE_VECTOR_MATH_SCALE;
  InitSocket(math_a, "A", blender::SOCK_VECTOR, &math_a_default);
  InitSocket(math_b, "B", blender::SOCK_VECTOR, &math_b_default);
  InitSocket(math_scale, "Scale", blender::SOCK_FLOAT, &scale_default);
  InitSocket(math_result, "Result", blender::SOCK_VECTOR, nullptr);
  Append(vector_math.inputs, math_a);
  Append(vector_math.inputs, math_b);
  Append(vector_math.inputs, math_scale);
  Append(vector_math.outputs, math_result);
  Append(tree.nodes, vector_math);

  blender::bNodeSocketValueVector position_default = {};
  blender::bNode set_position = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket position = {};
  SetObjectAttributeAuxSockets set_attribute_aux = {};
  InitSetObjectAttributeVectorNode(
      set_position, set_attribute_aux, flow, position, "Set Position", 5, 0, &position_default);
  Append(tree.nodes, set_position);

  blender::bNodeLink flow_link = {};
  blender::bNodeLink vector_to_math = {};
  blender::bNodeLink property_to_scale = {};
  blender::bNodeLink math_to_position = {};
  InitLink(flow_link, on_update, on_update_pulse, set_position, flow);
  InitLink(vector_to_math, vector_value, vector_value_output, vector_math, math_a);
  InitLink(property_to_scale, get_property, property_value, vector_math, math_scale);
  InitLink(math_to_position, vector_math, math_result, set_position, position);
  Append(tree.links, flow_link);
  Append(tree.links, vector_to_math);
  Append(tree.links, property_to_scale);
  Append(tree.links, math_to_position);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors());
  const LN_Instruction *instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::SetTransformVector);
  ASSERT_NE(instruction, nullptr);
  const LN_VectorExpression *expression = InstructionVectorExpression(*program, *instruction);
  ASSERT_NE(expression, nullptr);
  EXPECT_EQ(expression->kind, LN_VectorExpressionKind::Scale);
  const LN_FloatExpression *scale = FloatExpressionAt(*program, expression->float_expr_index);
  ASSERT_NE(scale, nullptr);
  EXPECT_EQ(scale->kind, LN_FloatExpressionKind::SnapshotGameProperty);
  EXPECT_TRUE(HasGamePropertyRef(*program, "scale", LN_ValueType::Float));
}

TEST(LN_TreeCompiler, DiagnosticsIncludeSourceTreeAndNodeIdentity)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode unknown = {};
  InitNode(unknown, "LogicNativeFuture", "Generated Node Name", 37);
  SetCString(unknown.label, "Designer Label");
  Append(tree.nodes, unknown);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  const LN_CompileReport &report = program->GetCompileReport();
  ASSERT_TRUE(report.HasErrors());
  ASSERT_FALSE(report.GetIssues().empty());
  EXPECT_EQ(report.GetSourceTreeName(), "CompilerProbe");
  EXPECT_EQ(report.GetDisabledReason(), "Logic Nodes compilation failed");

  const LN_CompileIssue &issue = report.GetIssues().front();
  ASSERT_LT(issue.source_ref_index, program->GetSourceRefs().size());
  const LN_SourceRef &source_ref = program->GetSourceRefs()[issue.source_ref_index];
  EXPECT_EQ(source_ref.source_tree_name, "CompilerProbe");
  EXPECT_EQ(source_ref.source_node_identifier, 37);
  EXPECT_EQ(source_ref.node_idname, "LogicNativeFuture");
  EXPECT_EQ(source_ref.node_name, "Designer Label");
  EXPECT_NE(issue.message.find("Unknown Logic Nodes node type"), std::string::npos);
}

TEST(LN_TreeCompiler, DiagnosticsIncludeSocketForMissingRequiredInput)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueFloat a_default = {};
  blender::bNode math = {};
  blender::bNodeSocket math_a = {};
  blender::bNodeSocket math_result = {};
  InitNode(math, "LogicNativeMath", "Math Missing B", 12);
  InitSocket(math_a, "A", blender::SOCK_FLOAT, &a_default);
  InitSocket(math_result, "Result", blender::SOCK_FLOAT, nullptr);
  Append(math.inputs, math_a);
  Append(math.outputs, math_result);
  Append(tree.nodes, math);

  blender::bNode print_node = {};
  blender::bNodeSocket print_flow = {};
  blender::bNodeSocket print_value = {};
  InitNode(print_node, "LogicNativePrint", "Print", 13);
  InitSocket(print_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocketWithIdentifier(print_value, "Message", "Value", blender::SOCK_CUSTOM, nullptr);
  Append(print_node.inputs, print_flow);
  Append(print_node.inputs, print_value);
  Append(tree.nodes, print_node);

  blender::bNodeLink math_to_print = {};
  blender::bNodeLink event_to_print = {};
  InitLink(math_to_print, math, math_result, print_node, print_value);
  InitLink(event_to_print, on_update, on_update_pulse, print_node, print_flow);
  Append(tree.links, math_to_print);
  Append(tree.links, event_to_print);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  ASSERT_TRUE(program->GetCompileReport().HasErrors());

  bool found_missing_b = false;
  for (const LN_CompileIssue &issue : program->GetCompileReport().GetIssues()) {
    ASSERT_LT(issue.source_ref_index, program->GetSourceRefs().size());
    const LN_SourceRef &source_ref = program->GetSourceRefs()[issue.source_ref_index];
    if (source_ref.node_idname == "LogicNativeMath" && source_ref.socket_name == "B") {
      found_missing_b = true;
      EXPECT_EQ(source_ref.node_name, "Math Missing B");
      EXPECT_NE(issue.message.find("Required input socket is missing"), std::string::npos);
      EXPECT_NE(issue.message.find("float data input 'B'"), std::string::npos);
    }
  }

  EXPECT_TRUE(found_missing_b);
}

TEST(LN_GamePropertyBindingSource, CollectsOneNativeAppliedTreeBinding)
{
  blender::Object object = {};
  blender::bNodeTree tree = {};
  InitTree(tree);
  SetCString(tree.id.name, "NTLogicProbe");
  blender::LogicNodeBinding binding = {};
  binding.tree = &tree;
  binding.enabled = 1;
  Append(object.logic_node_bindings, binding);

  LN_GameObjectBindingCandidate candidate;
  candidate.blender_object = &object;
  candidate.scene_object_index = 9;

  std::vector<LN_AppliedTreeDesc> applied_trees;
  LN_GamePropertyBindingSource({candidate}).CollectAppliedTrees(applied_trees);

  ASSERT_EQ(applied_trees.size(), 1);
  EXPECT_EQ(applied_trees[0].tree_name, "LogicProbe");
  EXPECT_EQ(applied_trees[0].scene_object_index, 9);
  EXPECT_EQ(applied_trees[0].applied_tree_index, 0);
  EXPECT_TRUE(applied_trees[0].enabled);
  EXPECT_EQ(applied_trees[0].tree, &tree);
}

TEST(LN_GamePropertyBindingSource, PreservesDisabledNativeBinding)
{
  blender::Object object = {};
  blender::bNodeTree tree = {};
  InitTree(tree);
  SetCString(tree.id.name, "NTLogicProbe");
  blender::LogicNodeBinding binding = {};
  binding.tree = &tree;
  binding.enabled = 0;
  Append(object.logic_node_bindings, binding);

  LN_GameObjectBindingCandidate candidate;
  candidate.blender_object = &object;

  std::vector<LN_AppliedTreeDesc> applied_trees;
  LN_GamePropertyBindingSource({candidate}).CollectAppliedTrees(applied_trees);

  ASSERT_EQ(applied_trees.size(), 1);
  EXPECT_FALSE(applied_trees[0].enabled);
}

TEST(LN_GamePropertyBindingSource, PreservesRuntimeActiveAndPerObjectAppliedIndex)
{
  blender::Object active_object = {};
  blender::Object inactive_object = {};
  blender::bNodeTree active_tree = {};
  blender::bNodeTree active_second_tree = {};
  blender::bNodeTree inactive_tree = {};
  InitTree(active_tree);
  InitTree(active_second_tree);
  InitTree(inactive_tree);
  SetCString(active_tree.id.name, "NTActiveTree");
  SetCString(active_second_tree.id.name, "NTActiveTreeSecond");
  SetCString(inactive_tree.id.name, "NTInactiveTree");
  blender::LogicNodeBinding active_binding = {};
  blender::LogicNodeBinding active_second_binding = {};
  blender::LogicNodeBinding inactive_binding = {};
  active_binding.tree = &active_tree;
  active_second_binding.tree = &active_second_tree;
  inactive_binding.tree = &inactive_tree;
  active_binding.enabled = 1;
  active_second_binding.enabled = 1;
  inactive_binding.enabled = 1;
  Append(active_object.logic_node_bindings, active_binding);
  Append(active_object.logic_node_bindings, active_second_binding);
  Append(inactive_object.logic_node_bindings, inactive_binding);

  LN_GameObjectBindingCandidate active_candidate;
  active_candidate.blender_object = &active_object;
  active_candidate.runtime_active = true;
  active_candidate.scene_object_index = 1;

  LN_GameObjectBindingCandidate inactive_candidate;
  inactive_candidate.blender_object = &inactive_object;
  inactive_candidate.runtime_active = false;
  inactive_candidate.scene_object_index = 2;

  std::vector<LN_AppliedTreeDesc> applied_trees;
  LN_GamePropertyBindingSource({active_candidate, inactive_candidate})
      .CollectAppliedTrees(applied_trees);

  ASSERT_EQ(applied_trees.size(), 3);
  EXPECT_EQ(applied_trees[0].applied_tree_index, 0);
  EXPECT_TRUE(applied_trees[0].runtime_active);
  EXPECT_EQ(applied_trees[1].applied_tree_index, 1);
  EXPECT_TRUE(applied_trees[1].runtime_active);
  EXPECT_EQ(applied_trees[2].applied_tree_index, 0);
  EXPECT_FALSE(applied_trees[2].runtime_active);
}

TEST(LN_GamePropertyBindingSource, UsesCurrentTreeNameAfterRename)
{
  blender::Object object = {};
  blender::bNodeTree tree = {};
  InitTree(tree);
  SetCString(tree.id.name, "NTRenamedTree");
  blender::LogicNodeBinding binding = {};
  SetCString(binding.tree_name, "ObsoleteTreeName");
  binding.tree = &tree;
  binding.enabled = 1;
  Append(object.logic_node_bindings, binding);

  LN_GameObjectBindingCandidate candidate;
  candidate.blender_object = &object;

  std::vector<LN_AppliedTreeDesc> applied_trees;
  LN_GamePropertyBindingSource({candidate}).CollectAppliedTrees(applied_trees);

  ASSERT_EQ(applied_trees.size(), 1);
  EXPECT_EQ(applied_trees[0].tree, &tree);
  EXPECT_EQ(applied_trees[0].tree_name, "RenamedTree");
}

TEST(LN_GamePropertyBindingSource, PreservesUnresolvedLegacyName)
{
  blender::Object object = {};
  blender::LogicNodeBinding binding = {};
  SetCString(binding.tree_name, "MissingLegacyTree");
  binding.flag = blender::LOGIC_NODE_BINDING_LEGACY_NAME_FALLBACK;
  binding.enabled = 1;
  Append(object.logic_node_bindings, binding);

  LN_GameObjectBindingCandidate candidate;
  candidate.blender_object = &object;

  std::vector<LN_AppliedTreeDesc> applied_trees;
  LN_GamePropertyBindingSource({candidate}).CollectAppliedTrees(applied_trees);

  ASSERT_EQ(applied_trees.size(), 1);
  EXPECT_EQ(applied_trees[0].tree, nullptr);
  EXPECT_EQ(applied_trees[0].tree_name, "MissingLegacyTree");
}

TEST(LN_GamePropertyBindingSource, SkipsObjectsWithoutNativeBindings)
{
  blender::Object object = {};
  LN_GameObjectBindingCandidate candidate;
  candidate.blender_object = &object;

  std::vector<LN_AppliedTreeDesc> applied_trees;
  LN_GamePropertyBindingSource({candidate}).CollectAppliedTrees(applied_trees);

  EXPECT_TRUE(applied_trees.empty());
}

static void init_sound_id_for_tests(blender::ID &sound_id, const char *name_without_prefix)
{
  memset(sound_id.name, 0, sizeof(sound_id.name));
  *reinterpret_cast<uint16_t *>(sound_id.name) = uint16_t(blender::ID_SO);
  strncpy(sound_id.name + 2, name_without_prefix, sizeof(sound_id.name) - 3);
  sound_id.name[sizeof(sound_id.name) - 1] = '\0';
}

TEST(LN_TreeCompiler, EmitsPlaySoundWithSoundNameConstant)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::ID sound_id = {};
  init_sound_id_for_tests(sound_id, "Coin");

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnFixedUpdate", "On Fixed Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueFloat volume_default = {};
  volume_default.value = 0.5f;
  blender::bNodeSocketValueFloat pitch_default = {};
  pitch_default.value = 1.25f;
  blender::bNodeSocketValueBoolean loop_default = {};
  loop_default.value = 1;
  blender::bNode play = {};
  blender::bNodeSocket play_flow = {};
  blender::bNodeSocket play_volume = {};
  blender::bNodeSocket play_pitch = {};
  blender::bNodeSocket play_loop = {};
  blender::bNodeSocket play_done = {};
  InitNode(play, "LogicNativePlaySound", "Play", 2);
  play.id = &sound_id;
  /* Group: Music. */
  play.custom2 = 1;
  InitSocket(play_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(play_volume, "Volume", blender::SOCK_FLOAT, &volume_default);
  InitSocket(play_pitch, "Pitch", blender::SOCK_FLOAT, &pitch_default);
  InitSocket(play_loop, "Loop", blender::SOCK_BOOLEAN, &loop_default);
  InitSocket(play_done, "Done", blender::SOCK_BOOLEAN, nullptr);
  Append(play.inputs, play_flow);
  Append(play.inputs, play_volume);
  Append(play.inputs, play_pitch);
  Append(play.inputs, play_loop);
  Append(play.outputs, play_done);
  Append(tree.nodes, play);

  blender::bNodeSocketValueVector position_default = {};
  position_default.value[0] = 1.0f;
  position_default.value[1] = 2.0f;
  position_default.value[2] = 3.0f;
  blender::bNode set_position = {};
  blender::bNodeSocket set_flow = {};
  blender::bNodeSocket set_position_socket = {};
  SetObjectAttributeAuxSockets set_attribute_aux = {};
  InitSetObjectAttributeVectorNode(set_position,
                                   set_attribute_aux,
                                   set_flow,
                                   set_position_socket,
                                   "Set Position",
                                   3,
                                   0,
                                   &position_default);
  Append(tree.nodes, set_position);

  blender::bNodeLink to_play = {};
  blender::bNodeLink done_to_set = {};
  InitLink(to_play, on_update, on_update_pulse, play, play_flow);
  InitLink(done_to_set, play, play_done, set_position, set_flow);
  Append(tree.links, to_play);
  Append(tree.links, done_to_set);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors());
  const LN_Instruction *play_instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::PlaySound);
  ASSERT_NE(play_instruction, nullptr);
  const LN_StringExpression *name_expr = StringExpressionAt(*program,
                                                            play_instruction->string_expr_index);
  ASSERT_NE(name_expr, nullptr);
  EXPECT_EQ(name_expr->kind, LN_StringExpressionKind::Constant);
  EXPECT_EQ(name_expr->string_value, "Coin");
  EXPECT_EQ(play_instruction->secondary_int_value, int32_t(LN_SoundGroup::Music));
}

TEST(LN_TreeCompiler, EmitsStopSoundOpcode)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::ID sound_id = {};
  init_sound_id_for_tests(sound_id, "Coin");

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnFixedUpdate", "On Fixed Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNode stop_sound = {};
  blender::bNodeSocket stop_flow = {};
  blender::bNodeSocket stop_done = {};
  InitNode(stop_sound, "LogicNativeStopSound", "Stop Sound", 2);
  stop_sound.id = &sound_id;
  InitSocket(stop_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(stop_done, "Done", blender::SOCK_BOOLEAN, nullptr);
  Append(stop_sound.inputs, stop_flow);
  Append(stop_sound.outputs, stop_done);
  Append(tree.nodes, stop_sound);

  blender::bNodeSocketValueVector position_default = {};
  position_default.value[0] = 4.0f;
  position_default.value[1] = 5.0f;
  position_default.value[2] = 6.0f;
  blender::bNode set_position = {};
  blender::bNodeSocket set_flow = {};
  blender::bNodeSocket set_position_socket = {};
  SetObjectAttributeAuxSockets set_attribute_aux = {};
  InitSetObjectAttributeVectorNode(set_position,
                                   set_attribute_aux,
                                   set_flow,
                                   set_position_socket,
                                   "Set Position",
                                   3,
                                   0,
                                   &position_default);
  Append(tree.nodes, set_position);

  blender::bNodeLink to_stop = {};
  blender::bNodeLink done_to_set = {};
  InitLink(to_stop, on_update, on_update_pulse, stop_sound, stop_flow);
  InitLink(done_to_set, stop_sound, stop_done, set_position, set_flow);
  Append(tree.links, to_stop);
  Append(tree.links, done_to_set);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors());
  const LN_Instruction *stop_instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::StopSound);
  ASSERT_NE(stop_instruction, nullptr);
  const LN_StringExpression *name_expr = StringExpressionAt(*program,
                                                            stop_instruction->string_expr_index);
  ASSERT_NE(name_expr, nullptr);
  EXPECT_EQ(name_expr->string_value, "Coin");
}

TEST(LN_TreeCompiler, EmitsPlaySound3DPauseAndResumeOpcodes)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::ID sound_id = {};
  init_sound_id_for_tests(sound_id, "TestSound");

  blender::bNode on_init = {};
  blender::bNodeSocket on_init_pulse = {};
  InitNode(on_init, "LogicNativeOnInit", "On Init", 1);
  InitSocket(on_init_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_init.outputs, on_init_pulse);
  Append(tree.nodes, on_init);

  blender::bNodeSocketValueFloat volume_default = {};
  volume_default.value = 0.75f;
  blender::bNodeSocketValueFloat pitch_default = {};
  pitch_default.value = 1.0f;
  blender::bNodeSocketValueBoolean loop_default = {};
  loop_default.value = 0;
  blender::bNode play_3d = {};
  blender::bNodeSocket play_flow = {};
  blender::bNodeSocket play_volume = {};
  blender::bNodeSocket play_pitch = {};
  blender::bNodeSocket play_loop = {};
  blender::bNodeSocket play_user = {};
  blender::bNodeSocketValueInt play_user_default = {};
  blender::bNodeSocket play_done = {};
  InitNode(play_3d, "LogicNativePlaySound3D", "Play 3D", 2);
  play_3d.id = &sound_id;
  /* Listener: Player (its User input, 0 here). */
  play_3d.custom1 = int16_t(LN_SoundListener::Player);
  InitSocket(play_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(play_volume, "Volume", blender::SOCK_FLOAT, &volume_default);
  InitSocket(play_pitch, "Pitch", blender::SOCK_FLOAT, &pitch_default);
  InitSocket(play_loop, "Loop", blender::SOCK_BOOLEAN, &loop_default);
  InitSocket(play_user, "User", blender::SOCK_INT, &play_user_default);
  InitSocket(play_done, "Done", blender::SOCK_BOOLEAN, nullptr);
  Append(play_3d.inputs, play_flow);
  Append(play_3d.inputs, play_volume);
  Append(play_3d.inputs, play_pitch);
  Append(play_3d.inputs, play_loop);
  Append(play_3d.inputs, play_user);
  Append(play_3d.outputs, play_done);
  Append(tree.nodes, play_3d);

  blender::bNode pause = {};
  blender::bNodeSocket pause_flow = {};
  blender::bNodeSocket pause_done = {};
  InitNode(pause, "LogicNativePauseSound", "Pause", 3);
  pause.id = &sound_id;
  InitSocket(pause_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(pause_done, "Done", blender::SOCK_BOOLEAN, nullptr);
  Append(pause.inputs, pause_flow);
  Append(pause.outputs, pause_done);
  Append(tree.nodes, pause);

  blender::bNode resume = {};
  blender::bNodeSocket resume_flow = {};
  blender::bNodeSocket resume_done = {};
  InitNode(resume, "LogicNativeResumeSound", "Resume", 4);
  resume.id = &sound_id;
  InitSocket(resume_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(resume_done, "Done", blender::SOCK_BOOLEAN, nullptr);
  Append(resume.inputs, resume_flow);
  Append(resume.outputs, resume_done);
  Append(tree.nodes, resume);

  blender::bNodeLink to_play = {};
  blender::bNodeLink play_to_pause = {};
  blender::bNodeLink pause_to_resume = {};
  InitLink(to_play, on_init, on_init_pulse, play_3d, play_flow);
  InitLink(play_to_pause, play_3d, play_done, pause, pause_flow);
  InitLink(pause_to_resume, pause, pause_done, resume, resume_flow);
  Append(tree.links, to_play);
  Append(tree.links, play_to_pause);
  Append(tree.links, pause_to_resume);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors());
  const LN_Instruction *play_instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnInit), LN_OpCode::PlaySound3D);
  const LN_Instruction *pause_instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnInit), LN_OpCode::PauseSound);
  const LN_Instruction *resume_instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnInit), LN_OpCode::ResumeSound);
  ASSERT_NE(play_instruction, nullptr);
  ASSERT_NE(pause_instruction, nullptr);
  ASSERT_NE(resume_instruction, nullptr);
  const LN_StringExpression *name_expr = StringExpressionAt(*program,
                                                            play_instruction->string_expr_index);
  ASSERT_NE(name_expr, nullptr);
  EXPECT_EQ(name_expr->string_value, "TestSound");
  EXPECT_EQ(play_instruction->int_value, int32_t(LN_SoundListener::Player));
  const LN_IntExpression *listener_user = IntExpressionAt(
      *program, play_instruction->secondary_int_expr_index);
  ASSERT_NE(listener_user, nullptr);
  EXPECT_EQ(listener_user->kind, LN_IntExpressionKind::Constant);
}

TEST(LN_TreeCompiler, EmitsSetGamePropertyFloatFromGetTreePropertyValue)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_init = {};
  blender::bNodeSocket on_init_pulse = {};
  InitNode(on_init, "LogicNativeOnInit", "On Init", 1);
  InitSocket(on_init_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_init.outputs, on_init_pulse);
  Append(tree.nodes, on_init);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 2);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueString tree_name_default = {};
  SetCString(tree_name_default.value, "tree_score");
  blender::bNodeSocketValueFloat tree_value_default = {};
  tree_value_default.value = 3.5f;
  blender::bNode set_tree = {};
  blender::bNodeSocket set_tree_condition = {};
  blender::bNodeSocket set_tree_name = {};
  blender::bNodeSocket set_tree_value = {};
  InitNode(set_tree, "LogicNativeSetTreeProperty", "Set Tree", 3);
  InitSocket(set_tree_condition, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(set_tree_name, "Property", blender::SOCK_STRING, &tree_name_default);
  InitSocket(set_tree_value, "Value", blender::SOCK_CUSTOM, nullptr);
  Append(set_tree.inputs, set_tree_condition);
  Append(set_tree.inputs, set_tree_name);
  Append(set_tree.inputs, set_tree_value);
  Append(tree.nodes, set_tree);

  blender::bNode value_float = {};
  blender::bNodeSocket value_float_out = {};
  InitNode(value_float, "LogicNativeValueFloat", "Value", 4);
  InitSocket(value_float_out, "Float", blender::SOCK_FLOAT, &tree_value_default);
  Append(value_float.outputs, value_float_out);
  Append(tree.nodes, value_float);

  blender::bNodeSocketValueString get_tree_name_default = {};
  SetCString(get_tree_name_default.value, "tree_score");
  blender::bNode get_tree = {};
  blender::bNodeSocket get_tree_name = {};
  blender::bNodeSocket get_tree_value = {};
  InitNode(get_tree, "LogicNativeGetTreeProperty", "Get Tree", 5);
  InitSocket(get_tree_name, "Property", blender::SOCK_STRING, &get_tree_name_default);
  InitSocket(get_tree_value, "Value", blender::SOCK_CUSTOM, nullptr);
  Append(get_tree.inputs, get_tree_name);
  Append(get_tree.outputs, get_tree_value);
  Append(tree.nodes, get_tree);

  blender::bNodeSocketValueString game_name_default = {};
  SetCString(game_name_default.value, "tree_value");
  blender::bNode set_game = {};
  blender::bNodeSocket set_game_flow = {};
  blender::bNodeSocket set_game_name = {};
  blender::bNodeSocket set_game_value = {};
  blender::bNodeSocket set_game_object = {};
  InitNode(set_game, "LogicNativeSetGamePropertyFloat", "Set Game", 6);
  InitSocket(set_game_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(set_game_object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(set_game_name, "Property", blender::SOCK_STRING, &game_name_default);
  InitSocket(set_game_value, "Value", blender::SOCK_FLOAT, nullptr);
  Append(set_game.inputs, set_game_flow);
  Append(set_game.inputs, set_game_object);
  Append(set_game.inputs, set_game_name);
  Append(set_game.inputs, set_game_value);
  Append(tree.nodes, set_game);

  blender::bNodeLink init_to_set_tree = {};
  blender::bNodeLink float_to_set_tree = {};
  blender::bNodeLink update_to_set_game = {};
  blender::bNodeLink tree_to_set_game = {};
  InitLink(init_to_set_tree, on_init, on_init_pulse, set_tree, set_tree_condition);
  InitLink(float_to_set_tree, value_float, value_float_out, set_tree, set_tree_value);
  InitLink(update_to_set_game, on_update, on_update_pulse, set_game, set_game_flow);
  InitLink(tree_to_set_game, get_tree, get_tree_value, set_game, set_game_value);
  Append(tree.links, init_to_set_tree);
  Append(tree.links, float_to_set_tree);
  Append(tree.links, update_to_set_game);
  Append(tree.links, tree_to_set_game);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors());
  const LN_Instruction *set_game_instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnUpdate), LN_OpCode::SetGameProperty);
  ASSERT_NE(set_game_instruction, nullptr);
  const LN_FloatExpression *float_expr = FloatExpressionAt(*program,
                                                           set_game_instruction->float_expr_index);
  ASSERT_NE(float_expr, nullptr);
  EXPECT_EQ(float_expr->kind, LN_FloatExpressionKind::FromGenericValue);
  const LN_ValueExpression *value_expr = ValueExpressionAt(*program, float_expr->input0);
  ASSERT_NE(value_expr, nullptr);
  EXPECT_EQ(value_expr->kind, LN_ValueExpressionKind::RuntimeTreeProperty);
  ASSERT_LT(value_expr->property_ref_index, program->GetTreePropertyRefs().size());
  EXPECT_EQ(program->GetTreePropertyRefs()[value_expr->property_ref_index].value_type,
            LN_ValueType::Generic);
}

TEST(LN_TreeCompiler, EmitsSetWorldPositionFromBoneHeadWorld)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnFixedUpdate", "On Fixed Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueString bone_name_default = {};
  SetCString(bone_name_default.value, "Root");
  blender::bNode get_bone = {};
  blender::bNodeSocket bone_object = {};
  blender::bNodeSocket bone_name = {};
  blender::bNodeSocket bone_position = {};
  InitNode(get_bone, "LogicNativeGetBoneHeadWorld", "Bone Head", 2);
  InitSocket(bone_object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(bone_name, "Bone Name", blender::SOCK_STRING, &bone_name_default);
  InitSocket(bone_position, "Position", blender::SOCK_VECTOR, nullptr);
  Append(get_bone.inputs, bone_object);
  Append(get_bone.inputs, bone_name);
  Append(get_bone.outputs, bone_position);
  Append(tree.nodes, get_bone);

  blender::bNodeSocketValueVector position_default = {};
  blender::bNode set_position = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket position = {};
  SetObjectAttributeAuxSockets set_attribute_aux = {};
  InitSetObjectAttributeVectorNode(
      set_position, set_attribute_aux, flow, position, "Set Position", 3, 0, &position_default);
  Append(tree.nodes, set_position);

  blender::bNodeLink flow_link = {};
  blender::bNodeLink position_link = {};
  InitLink(flow_link, on_update, on_update_pulse, set_position, flow);
  InitLink(position_link, get_bone, bone_position, set_position, position);
  Append(tree.links, flow_link);
  Append(tree.links, position_link);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors());
  const LN_Instruction *instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::SetTransformVector);
  ASSERT_NE(instruction, nullptr);
  const LN_VectorExpression *expression = InstructionVectorExpression(*program, *instruction);
  ASSERT_NE(expression, nullptr);
  EXPECT_EQ(expression->kind, LN_VectorExpressionKind::BoneHeadWorld);
  /* Unlinked Object socket: compile uses tree owner at runtime (input0 invalid). */
  EXPECT_EQ(expression->input0, LN_INVALID_INDEX);
  EXPECT_NE(expression->input1, LN_INVALID_INDEX);
}

TEST(LN_TreeCompiler, EmitsSetBonePoseLocationSpace)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueString bone_name_default = {};
  SetCString(bone_name_default.value, "Root");
  blender::bNodeSocketValueVector location_default = {};
  blender::bNode set_bone = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket object = {};
  blender::bNodeSocket bone_name = {};
  blender::bNodeSocket location = {};
  InitNode(set_bone, "LogicNativeSetBonePoseLocation", "Set Bone Pose Location", 2);
  set_bone.custom1 = int(LN_BonePoseLocationSpace::World);
  set_bone.custom2 = 1;
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(bone_name, "Bone Name", blender::SOCK_STRING, &bone_name_default);
  InitSocket(location, "Location", blender::SOCK_VECTOR, &location_default);
  Append(set_bone.inputs, flow);
  Append(set_bone.inputs, object);
  Append(set_bone.inputs, bone_name);
  Append(set_bone.inputs, location);
  Append(tree.nodes, set_bone);

  blender::bNodeLink flow_link = {};
  InitLink(flow_link, on_update, on_update_pulse, set_bone, flow);
  Append(tree.links, flow_link);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors());
  const LN_Instruction *instruction = FindInstruction(program->GetInstructions(LN_Event::OnUpdate),
                                                      LN_OpCode::SetBonePoseLocation);
  ASSERT_NE(instruction, nullptr);
  EXPECT_EQ(instruction->int_value, int(LN_BonePoseLocationSpace::World));
  EXPECT_EQ(instruction->secondary_int_value, 1);
}

TEST(LN_TreeCompiler, EmitsSetBonePoseRotationCenterPivot)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueString bone_name_default = {};
  SetCString(bone_name_default.value, "Root");
  blender::bNodeSocketValueRotation rotation_default = {};
  rotation_default.value_euler[1] = 0.25f;
  blender::bNode set_bone = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket object = {};
  blender::bNodeSocket bone_name = {};
  blender::bNodeSocket rotation = {};
  InitNode(set_bone, "LogicNativeSetBonePoseRotation", "Set Bone Pose Rotation", 2);
  set_bone.custom2 = 1;
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(bone_name, "Bone Name", blender::SOCK_STRING, &bone_name_default);
  InitSocket(rotation, "Rotation", blender::SOCK_ROTATION, &rotation_default);
  Append(set_bone.inputs, flow);
  Append(set_bone.inputs, object);
  Append(set_bone.inputs, bone_name);
  Append(set_bone.inputs, rotation);
  Append(tree.nodes, set_bone);

  blender::bNodeLink flow_link = {};
  InitLink(flow_link, on_update, on_update_pulse, set_bone, flow);
  Append(tree.links, flow_link);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors());
  const LN_Instruction *instruction = FindInstruction(program->GetInstructions(LN_Event::OnUpdate),
                                                      LN_OpCode::SetBonePoseRotation);
  ASSERT_NE(instruction, nullptr);
  EXPECT_EQ(instruction->secondary_int_value, 1);

  const LN_VectorExpression *expression = InstructionVectorExpression(*program, *instruction);
  ASSERT_NE(expression, nullptr);
  EXPECT_EQ(expression->output_type, LN_ValueType::Rotation);
  EXPECT_EQ(expression->vector_value.y(), 0.25f);
  EXPECT_LE(
      expression->rotation_value.AngleTo(LN_Rotation::FromEulerXYZ(MT_Vector3(0.0f, 0.25f, 0.0f))),
      0.0001f);
}

TEST(LN_TreeCompiler, EmitsSetBonePoseTransformSpaceAndCenter)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueString bone_name_default = {};
  SetCString(bone_name_default.value, "Root");
  blender::bNodeSocketValueVector location_default = {};
  location_default.value[0] = 1.0f;
  location_default.value[1] = 2.0f;
  location_default.value[2] = 3.0f;
  blender::bNodeSocketValueRotation rotation_default = {};
  rotation_default.value_euler[1] = 0.5f;
  blender::bNode set_bone = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket object = {};
  blender::bNodeSocket bone_name = {};
  blender::bNodeSocket location = {};
  blender::bNodeSocket rotation = {};
  InitNode(set_bone, "LogicNativeSetBonePoseTransform", "Set Bone Pose Transform", 2);
  set_bone.custom1 = int(LN_BonePoseLocationSpace::World);
  set_bone.custom2 = 1;
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(bone_name, "Bone Name", blender::SOCK_STRING, &bone_name_default);
  InitSocket(location, "Location", blender::SOCK_VECTOR, &location_default);
  InitSocket(rotation, "Rotation", blender::SOCK_ROTATION, &rotation_default);
  Append(set_bone.inputs, flow);
  Append(set_bone.inputs, object);
  Append(set_bone.inputs, bone_name);
  Append(set_bone.inputs, location);
  Append(set_bone.inputs, rotation);
  Append(tree.nodes, set_bone);

  blender::bNodeLink flow_link = {};
  InitLink(flow_link, on_update, on_update_pulse, set_bone, flow);
  Append(tree.links, flow_link);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors());
  const LN_Instruction *instruction = FindInstruction(program->GetInstructions(LN_Event::OnUpdate),
                                                      LN_OpCode::SetBonePoseTransform);
  ASSERT_NE(instruction, nullptr);
  EXPECT_EQ(instruction->int_value, int(LN_BonePoseLocationSpace::World));
  EXPECT_EQ(instruction->secondary_int_value, 1);

  const LN_VectorExpression *location_expression = InstructionVectorExpression(*program,
                                                                               *instruction);
  ASSERT_NE(location_expression, nullptr);
  EXPECT_EQ(location_expression->vector_value.z(), 3.0f);

  const LN_VectorExpression *rotation_expression = VectorExpressionAt(
      *program, instruction->secondary_vector_expr_index);
  ASSERT_NE(rotation_expression, nullptr);
  EXPECT_EQ(rotation_expression->output_type, LN_ValueType::Rotation);
  EXPECT_EQ(rotation_expression->vector_value.y(), 0.5f);
  EXPECT_LE(rotation_expression->rotation_value.AngleTo(
                LN_Rotation::FromEulerXYZ(MT_Vector3(0.0f, 0.5f, 0.0f))),
            0.0001f);
}

TEST(LN_TreeCompiler, RejectsLoopScopedCooldownFlowWithActionableDiagnostic)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket update_out = {};
  InitNode(on_update, "LogicNativeOnFixedUpdate", "On Fixed Update", 1);
  InitSocket(update_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, update_out);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueInt count_default = {};
  count_default.value = 2;
  blender::bNodeSocketValueInt ticks_default = {};
  ticks_default.value = 1;
  blender::bNode loop = {};
  blender::bNodeSocket loop_flow = {};
  blender::bNodeSocket loop_count = {};
  blender::bNodeSocket loop_ticks = {};
  blender::bNodeSocket loop_out = {};
  InitNode(loop, "LogicNativeLoop", "Loop", 2);
  InitSocket(loop_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(loop_count, "Count", blender::SOCK_INT, &count_default);
  InitSocket(loop_ticks, "Ticks to Complete", blender::SOCK_INT, &ticks_default);
  InitSocket(loop_out, "Loop", blender::SOCK_BOOLEAN, nullptr);
  Append(loop.inputs, loop_flow);
  Append(loop.inputs, loop_count);
  Append(loop.inputs, loop_ticks);
  Append(loop.outputs, loop_out);
  Append(tree.nodes, loop);

  blender::bNodeSocketValueFloat duration_default = {};
  duration_default.value = 0.5f;
  blender::bNodeSocketValueBoolean ignore_timescale_default = {};
  blender::bNode cooldown = {};
  blender::bNodeSocket cooldown_flow = {};
  blender::bNodeSocket cooldown_reset = {};
  blender::bNodeSocket cooldown_duration = {};
  blender::bNodeSocket cooldown_ignore_timescale = {};
  blender::bNodeSocket cooldown_accepted = {};
  blender::bNodeSocket cooldown_blocked = {};
  blender::bNodeSocket cooldown_completed = {};
  blender::bNodeSocket cooldown_ready = {};
  blender::bNodeSocket cooldown_remaining = {};
  blender::bNodeSocket cooldown_progress = {};
  InitNode(cooldown, "LogicNativeCooldown", "Cooldown", 3);
  InitSocket(cooldown_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(cooldown_reset, "Reset", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(cooldown_duration, "Duration", blender::SOCK_FLOAT, &duration_default);
  InitSocket(cooldown_ignore_timescale,
             "Ignore Timescale",
             blender::SOCK_BOOLEAN,
             &ignore_timescale_default);
  InitSocket(cooldown_accepted, "Accepted", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(cooldown_blocked, "Blocked", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(cooldown_completed, "Completed", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(cooldown_ready, "Is Ready", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(cooldown_remaining, "Remaining", blender::SOCK_FLOAT, nullptr);
  InitSocket(cooldown_progress, "Progress", blender::SOCK_FLOAT, nullptr);
  Append(cooldown.inputs, cooldown_flow);
  Append(cooldown.inputs, cooldown_reset);
  Append(cooldown.inputs, cooldown_duration);
  Append(cooldown.inputs, cooldown_ignore_timescale);
  Append(cooldown.outputs, cooldown_accepted);
  Append(cooldown.outputs, cooldown_blocked);
  Append(cooldown.outputs, cooldown_completed);
  Append(cooldown.outputs, cooldown_ready);
  Append(cooldown.outputs, cooldown_remaining);
  Append(cooldown.outputs, cooldown_progress);
  Append(tree.nodes, cooldown);

  blender::bNodeLink update_to_loop = {};
  blender::bNodeLink loop_to_cooldown = {};
  InitLink(update_to_loop, on_update, update_out, loop, loop_flow);
  InitLink(loop_to_cooldown, loop, loop_out, cooldown, cooldown_flow);
  Append(tree.links, update_to_loop);
  Append(tree.links, loop_to_cooldown);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  const LN_CompileReport &report = program->GetCompileReport();
  EXPECT_TRUE(report.HasErrors()) << FormatCompileReport(report);
  EXPECT_TRUE(HasErrorContaining(report, "Cooldown cannot run inside a Loop body"))
      << FormatCompileReport(report);
  EXPECT_TRUE(HasErrorContaining(report, "trigger it from a separate non-loop event path"))
      << FormatCompileReport(report);
  EXPECT_EQ(FindInstruction(program->GetInstructions(LN_Event::OnFixedUpdate),
                            LN_OpCode::TryCooldown),
            nullptr);
}

TEST(LN_TreeCompiler, RoutedDoOncePulseUsesOnlyLoopActiveForTheLoopBody)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket update_out = {};
  InitNode(on_update, "LogicNativeOnFixedUpdate", "On Fixed Update", 1);
  InitSocket(update_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, update_out);
  Append(tree.nodes, on_update);

  blender::bNode once = {};
  blender::bNodeSocket once_flow = {};
  blender::bNodeSocket once_reset = {};
  blender::bNodeSocket once_out = {};
  InitNode(once, "LogicNativeOnce", "Do Once", 2);
  InitSocket(once_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(once_reset, "Reset", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(once_out, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(once.inputs, once_flow);
  Append(once.inputs, once_reset);
  Append(once.outputs, once_out);
  Append(tree.nodes, once);

  blender::bNodeSocketValueInt count_default = {};
  count_default.value = 3;
  blender::bNodeSocketValueInt ticks_default = {};
  ticks_default.value = 1;
  blender::bNode loop = {};
  blender::bNodeSocket loop_flow = {};
  blender::bNodeSocket loop_count = {};
  blender::bNodeSocket loop_ticks = {};
  blender::bNodeSocket loop_out = {};
  InitNode(loop, "LogicNativeLoop", "Loop", 3);
  InitSocket(loop_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(loop_count, "Count", blender::SOCK_INT, &count_default);
  InitSocket(loop_ticks, "Ticks to Complete", blender::SOCK_INT, &ticks_default);
  InitSocket(loop_out, "Loop", blender::SOCK_BOOLEAN, nullptr);
  Append(loop.inputs, loop_flow);
  Append(loop.inputs, loop_count);
  Append(loop.inputs, loop_ticks);
  Append(loop.outputs, loop_out);
  Append(tree.nodes, loop);

  blender::bNodeSocketValueString message_default = {};
  SetCString(message_default.value, "iteration");
  blender::bNode print_node = {};
  blender::bNodeSocket print_flow = {};
  blender::bNodeSocket print_value = {};
  InitNode(print_node, "LogicNativePrint", "Print", 4);
  InitSocket(print_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(print_value, "Value", blender::SOCK_STRING, &message_default);
  Append(print_node.inputs, print_flow);
  Append(print_node.inputs, print_value);
  Append(tree.nodes, print_node);

  blender::bNodeLink update_to_once = {};
  blender::bNodeLink once_to_loop = {};
  blender::bNodeLink loop_to_print = {};
  InitLink(update_to_once, on_update, update_out, once, once_flow);
  InitLink(once_to_loop, once, once_out, loop, loop_flow);
  InitLink(loop_to_print, loop, loop_out, print_node, print_flow);
  Append(tree.links, update_to_once);
  Append(tree.links, once_to_loop);
  Append(tree.links, loop_to_print);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  ASSERT_EQ(program->GetLoopFrames().size(), 1u);

  const LN_LoopFrame &frame = program->GetLoopFrames().front();
  const LN_BoolExpression *trigger = BoolExpressionAt(*program, frame.trigger_bool_expr_index);
  ASSERT_NE(trigger, nullptr);
  EXPECT_EQ(trigger->kind, LN_BoolExpressionKind::InstructionExecuted);

  const std::vector<LN_Instruction> &fixed = program->GetInstructions(LN_Event::OnFixedUpdate);
  ASSERT_LT(trigger->input0, fixed.size());
  EXPECT_EQ(fixed[trigger->input0].opcode, LN_OpCode::BranchRoute);

  const LN_Instruction *print_instruction = FindInstruction(fixed, LN_OpCode::Print);
  ASSERT_NE(print_instruction, nullptr);
  EXPECT_EQ(print_instruction->loop_frame_index, 0u);
  EXPECT_EQ(print_instruction->bool_guard_expr_index, frame.loop_active_bool_expr_index);
  const LN_BoolExpression *body_guard = BoolExpressionAt(*program,
                                                         print_instruction->bool_guard_expr_index);
  ASSERT_NE(body_guard, nullptr);
  EXPECT_EQ(body_guard->kind, LN_BoolExpressionKind::LoopActive);
  EXPECT_EQ(body_guard->int_value, 0);
}

TEST(LN_TreeCompiler, EmitsLoopBodyInstructions)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnFixedUpdate", "On Fixed Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueInt count_default = {};
  count_default.value = 2;
  blender::bNodeSocketValueInt ticks_default = {};
  ticks_default.value = 1;
  blender::bNode loop = {};
  blender::bNodeSocket loop_condition = {};
  blender::bNodeSocket loop_count = {};
  blender::bNodeSocket loop_ticks = {};
  blender::bNodeSocket loop_out = {};
  InitNode(loop, "LogicNativeLoop", "Loop", 2);
  InitSocket(loop_condition, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(loop_count, "Count", blender::SOCK_INT, &count_default);
  InitSocket(loop_ticks, "Ticks to Complete", blender::SOCK_INT, &ticks_default);
  InitSocket(loop_out, "Loop", blender::SOCK_BOOLEAN, nullptr);
  Append(loop.inputs, loop_condition);
  Append(loop.inputs, loop_count);
  Append(loop.inputs, loop_ticks);
  Append(loop.outputs, loop_out);
  Append(tree.nodes, loop);

  blender::bNodeSocketValueString message_default = {};
  SetCString(message_default.value, "tick");
  blender::bNode print = {};
  blender::bNodeSocket print_flow = {};
  blender::bNodeSocket print_message = {};
  InitNode(print, "LogicNativePrint", "Print", 3);
  InitSocket(print_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(print_message, "Message", blender::SOCK_STRING, &message_default);
  Append(print.inputs, print_flow);
  Append(print.inputs, print_message);
  Append(tree.nodes, print);

  blender::bNodeLink to_loop = {};
  blender::bNodeLink loop_flow = {};
  InitLink(to_loop, on_update, on_update_pulse, loop, loop_condition);
  InitLink(loop_flow, loop, loop_out, print, print_flow);
  Append(tree.links, to_loop);
  Append(tree.links, loop_flow);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors());
  EXPECT_EQ(program->GetLoopFrames().size(), 1u);

  const LN_Instruction *print_instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::Print);
  ASSERT_NE(print_instruction, nullptr);
  EXPECT_EQ(print_instruction->loop_frame_index, 0u);
}

TEST(LN_TreeCompiler, SetParentCompilesModernBoneContract)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::Object parent_object = {};
  SetCString(parent_object.id.name, "OBArmature");

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnFixedUpdate", "On Fixed Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueObject child_default = {};
  blender::bNodeSocketValueObject parent_default = {};
  blender::bNodeSocketValueString bone_name_default = {};
  blender::bNodeSocketValueBoolean compound_default = {};
  blender::bNodeSocketValueBoolean ghost_default = {};
  parent_default.value = &parent_object;
  SetCString(bone_name_default.value, "hand.R");

  blender::bNode set_parent = {};
  SetParentNodeSockets sockets;
  InitSetParentNode(set_parent,
                    sockets,
                    child_default,
                    parent_default,
                    bone_name_default,
                    compound_default,
                    ghost_default,
                    2);
  constexpr int settings_initialized = 1 << 14;
  set_parent.custom1 = 1; /* Bone. */
  set_parent.custom2 = settings_initialized;
  Append(tree.nodes, set_parent);

  blender::bNodeLink flow_link = {};
  InitLink(flow_link, on_update, on_update_pulse, set_parent, sockets.flow);
  Append(tree.links, flow_link);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  const LN_Instruction *instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::SetParent);
  ASSERT_NE(instruction, nullptr);
  EXPECT_TRUE(instruction->bool_value);
  EXPECT_EQ(instruction->int_value, 1);
  EXPECT_EQ(instruction->secondary_int_value, 0);
  EXPECT_EQ(instruction->bool_expr_index, LN_INVALID_INDEX);
  EXPECT_EQ(instruction->secondary_bool_expr_index, LN_INVALID_INDEX);
  EXPECT_NE(instruction->secondary_value_expr_index, LN_INVALID_INDEX);
  EXPECT_EQ(instruction->vector_expr_index, LN_INVALID_INDEX);
  EXPECT_EQ(instruction->secondary_vector_expr_index, LN_INVALID_INDEX);
  EXPECT_EQ(instruction->tertiary_value_expr_index, LN_INVALID_INDEX);

  const LN_StringExpression *bone_name = StringExpressionAt(*program,
                                                            instruction->string_expr_index);
  ASSERT_NE(bone_name, nullptr);
  EXPECT_EQ(bone_name->kind, LN_StringExpressionKind::Constant);
  EXPECT_EQ(bone_name->string_value, "hand.R");
}

TEST(LN_TreeCompiler, SetParentCompilesModernObjectCompoundContract)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::Object parent_object = {};
  SetCString(parent_object.id.name, "OBParent");

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnFixedUpdate", "On Fixed Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueObject child_default = {};
  blender::bNodeSocketValueObject parent_default = {};
  blender::bNodeSocketValueString bone_name_default = {};
  blender::bNodeSocketValueBoolean compound_default = {};
  blender::bNodeSocketValueBoolean ghost_default = {};
  parent_default.value = &parent_object;
  compound_default.value = 1;

  blender::bNode set_parent = {};
  SetParentNodeSockets sockets;
  InitSetParentNode(set_parent,
                    sockets,
                    child_default,
                    parent_default,
                    bone_name_default,
                    compound_default,
                    ghost_default,
                    2);
  constexpr int settings_initialized = 1 << 14;
  constexpr int compound_initialized = 1 << 13;
  set_parent.custom1 = 0; /* Object. */
  set_parent.custom2 = settings_initialized | compound_initialized;
  Append(tree.nodes, set_parent);

  blender::bNodeLink flow_link = {};
  InitLink(flow_link, on_update, on_update_pulse, set_parent, sockets.flow);
  Append(tree.links, flow_link);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  const LN_Instruction *instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::SetParent);
  ASSERT_NE(instruction, nullptr);
  EXPECT_TRUE(instruction->bool_value);
  EXPECT_EQ(instruction->int_value, 0);
  EXPECT_EQ(instruction->string_expr_index, LN_INVALID_INDEX);

  const LN_BoolExpression *compound = BoolExpressionAt(*program, instruction->bool_expr_index);
  ASSERT_NE(compound, nullptr);
  EXPECT_EQ(compound->kind, LN_BoolExpressionKind::Constant);
  EXPECT_TRUE(compound->bool_value);
}

TEST(LN_TreeCompiler, SetParentKeepsLegacyCompoundAndGhostInstructionPayload)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::Object parent_object = {};
  SetCString(parent_object.id.name, "OBParent");

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnFixedUpdate", "On Fixed Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueObject child_default = {};
  blender::bNodeSocketValueObject parent_default = {};
  blender::bNodeSocketValueString bone_name_default = {};
  blender::bNodeSocketValueBoolean compound_default = {};
  blender::bNodeSocketValueBoolean ghost_default = {};
  parent_default.value = &parent_object;
  compound_default.value = 1;
  ghost_default.value = 0;

  blender::bNode set_parent = {};
  SetParentNodeSockets sockets;
  InitSetParentNode(set_parent,
                    sockets,
                    child_default,
                    parent_default,
                    bone_name_default,
                    compound_default,
                    ghost_default,
                    2);
  set_parent.custom1 = 1; /* Ignored by legacy nodes. */
  set_parent.custom2 = 0; /* No modern-settings marker. */
  Append(tree.nodes, set_parent);

  blender::bNodeLink flow_link = {};
  InitLink(flow_link, on_update, on_update_pulse, set_parent, sockets.flow);
  Append(tree.links, flow_link);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  const LN_Instruction *instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::SetParent);
  ASSERT_NE(instruction, nullptr);
  EXPECT_FALSE(instruction->bool_value);
  EXPECT_EQ(instruction->string_expr_index, LN_INVALID_INDEX);
  EXPECT_EQ(instruction->vector_expr_index, LN_INVALID_INDEX);
  EXPECT_EQ(instruction->secondary_vector_expr_index, LN_INVALID_INDEX);
  EXPECT_EQ(instruction->tertiary_value_expr_index, LN_INVALID_INDEX);

  const LN_BoolExpression *compound = BoolExpressionAt(*program, instruction->bool_expr_index);
  const LN_BoolExpression *ghost = BoolExpressionAt(*program,
                                                    instruction->secondary_bool_expr_index);
  ASSERT_NE(compound, nullptr);
  ASSERT_NE(ghost, nullptr);
  EXPECT_EQ(compound->kind, LN_BoolExpressionKind::Constant);
  EXPECT_TRUE(compound->bool_value);
  EXPECT_EQ(ghost->kind, LN_BoolExpressionKind::Constant);
  EXPECT_FALSE(ghost->bool_value);
}

TEST(LN_TreeCompiler, SpatialAnalysisNodesCompileEverySocketContract)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket update_out = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(update_out, "Out", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  Append(on_update.outputs, update_out);
  Append(tree.nodes, on_update);

  blender::bNode on_init = {};
  blender::bNodeSocket init_out = {};
  InitNode(on_init, "LogicNativeOnInit", "On Init", 2);
  InitSocket(init_out, "Out", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  Append(on_init.outputs, init_out);
  Append(tree.nodes, on_init);

  blender::bNode average = {};
  blender::bNodeSocket vectors = {};
  blender::bNodeSocket objects = {};
  blender::bNodeSocket collection = {};
  blender::bNodeSocket center_out = {};
  blender::bNodeSocket count_out = {};
  blender::bNodeSocket average_valid_out = {};
  InitNode(average,
           "LogicNativeGetAverageCenterPosition",
           "Get Average Center Position",
           3);
  average.custom1 = int32_t(LN_AverageCenterSource::VectorList);
  InitSocket(vectors, "Vectors", blender::SOCK_CUSTOM, nullptr, "NodeSocketLogicList");
  InitSocket(objects, "Objects", blender::SOCK_CUSTOM, nullptr, "NodeSocketLogicList");
  InitSocket(collection, "Collection", blender::SOCK_COLLECTION, nullptr);
  InitSocket(center_out, "Center", blender::SOCK_VECTOR, nullptr);
  InitSocket(count_out, "Count", blender::SOCK_INT, nullptr);
  InitSocket(average_valid_out, "Valid", blender::SOCK_BOOLEAN, nullptr);
  Append(average.inputs, vectors);
  Append(average.inputs, objects);
  Append(average.inputs, collection);
  Append(average.outputs, center_out);
  Append(average.outputs, count_out);
  Append(average.outputs, average_valid_out);
  Append(tree.nodes, average);

  blender::bNodeSocketValueObject object_a_default = {};
  blender::bNodeSocketValueObject object_b_default = {};
  blender::bNodeSocketValueVector position_a_default = {};
  blender::bNodeSocketValueVector position_b_default = {};
  position_a_default.value[0] = 1.0f;
  position_b_default.value[1] = 2.0f;
  blender::bNode distance = {};
  blender::bNodeSocket object_a = {};
  blender::bNodeSocket position_a = {};
  blender::bNodeSocket object_b = {};
  blender::bNodeSocket position_b = {};
  blender::bNodeSocket distance_out = {};
  blender::bNodeSocket squared_distance_out = {};
  blender::bNodeSocket distance_valid_out = {};
  InitNode(distance, "LogicNativeGetDistance", "Distance Between", 4);
  distance.custom1 = 1;
  distance.custom2 = 1;
  InitSocket(object_a, "Object A", blender::SOCK_OBJECT, &object_a_default);
  InitSocket(position_a, "Position A", blender::SOCK_VECTOR, &position_a_default);
  InitSocket(object_b, "Object B", blender::SOCK_OBJECT, &object_b_default);
  InitSocket(position_b, "Position B", blender::SOCK_VECTOR, &position_b_default);
  InitSocket(distance_out, "Distance", blender::SOCK_FLOAT, nullptr);
  InitSocket(squared_distance_out, "Squared Distance", blender::SOCK_FLOAT, nullptr);
  InitSocket(distance_valid_out, "Valid", blender::SOCK_BOOLEAN, nullptr);
  Append(distance.inputs, object_a);
  Append(distance.inputs, position_a);
  Append(distance.inputs, object_b);
  Append(distance.inputs, position_b);
  Append(distance.outputs, distance_out);
  Append(distance.outputs, squared_distance_out);
  Append(distance.outputs, distance_valid_out);
  Append(tree.nodes, distance);

  blender::bNodeSocketValueObject motion_object_default = {};
  blender::bNodeSocketValueVector motion_position_default = {};
  blender::bNodeSocketValueFloat prediction_time_default = {};
  blender::bNodeSocketValueFloat smoothing_time_default = {};
  prediction_time_default.value = 0.5f;
  smoothing_time_default.value = 0.1f;
  blender::bNode motion = {};
  blender::bNodeSocket motion_flow = {};
  blender::bNodeSocket motion_reset = {};
  blender::bNodeSocket motion_object = {};
  blender::bNodeSocket motion_position = {};
  blender::bNodeSocket prediction_time = {};
  blender::bNodeSocket smoothing_time = {};
  blender::bNodeSocket updated = {};
  blender::bNodeSocket sampled_position = {};
  blender::bNodeSocket velocity = {};
  blender::bNodeSocket direction = {};
  blender::bNodeSocket speed = {};
  blender::bNodeSocket predicted_position = {};
  blender::bNodeSocket motion_valid = {};
  InitNode(motion, "LogicNativePredictMotion", "Predict Motion", 5);
  motion.custom1 = 1;
  InitSocket(motion_flow, "Flow", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  InitSocket(motion_reset, "Reset", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  InitSocket(motion_object, "Object", blender::SOCK_OBJECT, &motion_object_default);
  InitSocket(motion_position, "Position", blender::SOCK_VECTOR, &motion_position_default);
  InitSocket(prediction_time, "Prediction Time", blender::SOCK_FLOAT, &prediction_time_default);
  InitSocket(smoothing_time, "Smoothing Time", blender::SOCK_FLOAT, &smoothing_time_default);
  InitSocket(updated, "Updated", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  InitSocket(sampled_position, "Position", blender::SOCK_VECTOR, nullptr);
  InitSocket(velocity, "Velocity", blender::SOCK_VECTOR, nullptr);
  InitSocket(direction, "Direction", blender::SOCK_VECTOR, nullptr);
  InitSocket(speed, "Speed", blender::SOCK_FLOAT, nullptr);
  InitSocket(predicted_position, "Predicted Position", blender::SOCK_VECTOR, nullptr);
  InitSocket(motion_valid, "Valid", blender::SOCK_BOOLEAN, nullptr);
  Append(motion.inputs, motion_flow);
  Append(motion.inputs, motion_reset);
  Append(motion.inputs, motion_object);
  Append(motion.inputs, motion_position);
  Append(motion.inputs, prediction_time);
  Append(motion.inputs, smoothing_time);
  Append(motion.outputs, updated);
  Append(motion.outputs, sampled_position);
  Append(motion.outputs, velocity);
  Append(motion.outputs, direction);
  Append(motion.outputs, speed);
  Append(motion.outputs, predicted_position);
  Append(motion.outputs, motion_valid);
  Append(tree.nodes, motion);

  blender::bNodeLink update_to_motion = {};
  blender::bNodeLink init_to_reset = {};
  InitLink(update_to_motion, on_update, update_out, motion, motion_flow);
  InitLink(init_to_reset, on_init, init_out, motion, motion_reset);
  Append(tree.links, update_to_motion);
  Append(tree.links, init_to_reset);

  std::array<blender::bNode, 12> print_nodes = {};
  std::array<blender::bNodeSocket, 12> print_flows = {};
  std::array<blender::bNodeSocket, 12> print_values = {};
  std::array<blender::bNodeLink, 12> flow_links = {};
  std::array<blender::bNodeLink, 12> value_links = {};
  size_t print_index = 0;
  auto consume_output = [&](blender::bNode &source,
                            blender::bNodeSocket &source_socket,
                            blender::bNode &flow_node,
                            blender::bNodeSocket &flow_source) {
    const size_t index = print_index++;
    InitNode(print_nodes[index], "LogicNativePrint", "Print Spatial Output", 100 + int32_t(index));
    InitSocket(
        print_flows[index], "Flow", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
    InitSocketWithIdentifier(print_values[index],
                             "Message",
                             "Value",
                             blender::SOCK_CUSTOM,
                             nullptr,
                             "NodeSocketLogicGeneric");
    Append(print_nodes[index].inputs, print_flows[index]);
    Append(print_nodes[index].inputs, print_values[index]);
    Append(tree.nodes, print_nodes[index]);
    InitLink(flow_links[index], flow_node, flow_source, print_nodes[index], print_flows[index]);
    InitLink(value_links[index], source, source_socket, print_nodes[index], print_values[index]);
    Append(tree.links, flow_links[index]);
    Append(tree.links, value_links[index]);
  };
  consume_output(average, center_out, on_update, update_out);
  consume_output(average, count_out, on_update, update_out);
  consume_output(average, average_valid_out, on_update, update_out);
  consume_output(distance, distance_out, on_update, update_out);
  consume_output(distance, squared_distance_out, on_update, update_out);
  consume_output(distance, distance_valid_out, on_update, update_out);
  consume_output(motion, sampled_position, motion, updated);
  consume_output(motion, velocity, motion, updated);
  consume_output(motion, direction, motion, updated);
  consume_output(motion, speed, motion, updated);
  consume_output(motion, predicted_position, motion, updated);
  consume_output(motion, motion_valid, motion, updated);
  ASSERT_EQ(print_index, print_nodes.size());

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(program, nullptr);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  int vector_average_queries = 0;
  int distance_queries = 0;
  for (const LN_QueryExpression &query : program->GetQueryExpressions()) {
    if (query.kind == LN_QueryExpressionKind::VectorAverage) {
      vector_average_queries++;
      EXPECT_EQ(query.average_center_source, LN_AverageCenterSource::VectorList);
    }
    if (query.kind == LN_QueryExpressionKind::DistanceBetween) {
      distance_queries++;
      EXPECT_EQ(query.distance_endpoint_a, LN_DistanceEndpointMode::Position);
      EXPECT_EQ(query.distance_endpoint_b, LN_DistanceEndpointMode::Position);
      EXPECT_NE(query.input0, LN_INVALID_INDEX);
      EXPECT_NE(query.input1, LN_INVALID_INDEX);
    }
  }
  EXPECT_EQ(vector_average_queries, 1);
  EXPECT_EQ(distance_queries, 1);

  const LN_Instruction *sample = FindInstruction(program->GetInstructions(LN_Event::OnUpdate),
                                                 LN_OpCode::SampleMotion);
  const LN_Instruction *reset = FindInstruction(program->GetInstructions(LN_Event::OnInit),
                                                LN_OpCode::ResetMotion);
  ASSERT_NE(sample, nullptr);
  ASSERT_NE(reset, nullptr);
  EXPECT_EQ(sample->secondary_int_value, int32_t(LN_MotionSourceMode::Position));
  EXPECT_NE(sample->vector_expr_index, LN_INVALID_INDEX);
  EXPECT_NE(sample->float_expr_index, LN_INVALID_INDEX);
  EXPECT_NE(sample->secondary_float_expr_index, LN_INVALID_INDEX);
  EXPECT_EQ(sample->int_value, reset->int_value);

  const auto count_kind = [](const auto &expressions, const auto kind) {
    return std::count_if(expressions.begin(), expressions.end(), [&](const auto &expression) {
      return expression.kind == kind;
    });
  };
  EXPECT_EQ(
      count_kind(program->GetVectorExpressions(), LN_VectorExpressionKind::QueryVectorAverage), 1);
  EXPECT_EQ(count_kind(program->GetIntExpressions(), LN_IntExpressionKind::QueryVectorCount), 1);
  EXPECT_EQ(count_kind(program->GetFloatExpressions(), LN_FloatExpressionKind::QueryDistance), 1);
  EXPECT_EQ(
      count_kind(program->GetFloatExpressions(), LN_FloatExpressionKind::QuerySquaredDistance), 1);
  EXPECT_GE(count_kind(program->GetBoolExpressions(), LN_BoolExpressionKind::QueryValid), 2);
  EXPECT_EQ(count_kind(program->GetVectorExpressions(), LN_VectorExpressionKind::MotionPosition),
            1);
  EXPECT_EQ(count_kind(program->GetVectorExpressions(), LN_VectorExpressionKind::MotionVelocity),
            1);
  EXPECT_EQ(count_kind(program->GetVectorExpressions(), LN_VectorExpressionKind::MotionDirection),
            1);
  EXPECT_EQ(count_kind(program->GetVectorExpressions(),
                       LN_VectorExpressionKind::MotionPredictedPosition),
            1);
  EXPECT_EQ(count_kind(program->GetFloatExpressions(), LN_FloatExpressionKind::MotionSpeed), 1);
  EXPECT_EQ(count_kind(program->GetBoolExpressions(), LN_BoolExpressionKind::MotionValid), 1);

  /* Reset is an optional control path. Removing it must leave a valid Predict Motion program. */
  Remove(tree.links, init_to_reset);
  const std::shared_ptr<LN_Program> without_reset = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(without_reset, nullptr);
  EXPECT_FALSE(without_reset->GetCompileReport().HasErrors())
      << FormatCompileReport(without_reset->GetCompileReport());
  EXPECT_TRUE(without_reset->ValidateInstructionPayloads());
  EXPECT_EQ(FindInstruction(without_reset->GetInstructions(LN_Event::OnInit),
                            LN_OpCode::ResetMotion),
            nullptr);
  EXPECT_NE(FindInstruction(without_reset->GetInstructions(LN_Event::OnUpdate),
                            LN_OpCode::SampleMotion),
            nullptr);
}

TEST(LN_TreeCompiler, HasPropertyReadsTheObjectOfAUIEvent)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNodeSocketValueObject object_default = {};
  blender::bNodeSocketValueString property_name_default = {};
  blender::bNodeSocketValueString action_default = {};
  SetCString(property_name_default.value, "exit");

  blender::bNode object_ui = {};
  blender::bNodeSocket action_input = {};
  blender::bNodeSocket on_press = {};
  blender::bNodeSocket target_object = {};
  InitNode(object_ui, "LogicNativeOnUIEvent", "On UI Event", 1);
  object_ui.custom1 = int16_t(LN_UIElementEvent::Press);
  InitSocket(action_input, "Action", blender::SOCK_STRING, &action_default);
  InitSocket(on_press, "Out", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  InitSocket(target_object, "Object", blender::SOCK_OBJECT, nullptr);
  Append(object_ui.inputs, action_input);
  Append(object_ui.outputs, on_press);
  Append(object_ui.outputs, target_object);
  Append(tree.nodes, object_ui);

  blender::bNode has_property = {};
  blender::bNodeSocket property_object = {};
  blender::bNodeSocket property_name = {};
  blender::bNodeSocket property_exists = {};
  InitNode(has_property, "LogicNativeHasProperty", "Has Property", 2);
  InitSocket(property_object, "Object", blender::SOCK_OBJECT, &object_default);
  InitSocket(property_name, "Name", blender::SOCK_STRING, &property_name_default);
  InitSocketWithIdentifier(
      property_exists, "Out", "If True", blender::SOCK_BOOLEAN, nullptr);
  Append(has_property.inputs, property_object);
  Append(has_property.inputs, property_name);
  Append(has_property.outputs, property_exists);
  Append(tree.nodes, has_property);

  blender::bNodeSocketValueBoolean branch_default = {};
  blender::bNode branch = {};
  blender::bNodeSocket branch_flow = {};
  blender::bNodeSocket branch_condition = {};
  blender::bNodeSocket branch_true = {};
  blender::bNodeSocket branch_false = {};
  InitNode(branch, "LogicNativeBranch", "Branch", 3);
  InitSocket(branch_flow, "Flow", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  InitSocket(branch_condition, "Condition", blender::SOCK_BOOLEAN, &branch_default);
  InitSocket(branch_true, "True", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  InitSocket(branch_false, "False", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  Append(branch.inputs, branch_flow);
  Append(branch.inputs, branch_condition);
  Append(branch.outputs, branch_true);
  Append(branch.outputs, branch_false);
  Append(tree.nodes, branch);

  blender::bNode quit = {};
  blender::bNodeSocket quit_flow = {};
  InitNode(quit, "LogicNativeQuitGame", "Quit Game", 4);
  InitSocket(quit_flow, "Flow", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  Append(quit.inputs, quit_flow);
  Append(tree.nodes, quit);

  blender::bNodeLink press_to_branch = {};
  blender::bNodeLink object_to_property = {};
  blender::bNodeLink property_to_branch = {};
  blender::bNodeLink branch_to_quit = {};
  InitLink(press_to_branch, object_ui, on_press, branch, branch_flow);
  InitLink(object_to_property, object_ui, target_object, has_property, property_object);
  InitLink(property_to_branch, has_property, property_exists, branch, branch_condition);
  InitLink(branch_to_quit, branch, branch_true, quit, quit_flow);
  Append(tree.links, press_to_branch);
  Append(tree.links, object_to_property);
  Append(tree.links, property_to_branch);
  Append(tree.links, branch_to_quit);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(program, nullptr);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());

  uint32_t target_expression_index = LN_INVALID_INDEX;
  uint32_t property_expression_index = LN_INVALID_INDEX;
  for (uint32_t index = 0; index < program->GetValueExpressions().size(); ++index) {
    const LN_ValueExpression &expression = program->GetValueExpressions()[index];
    if (expression.kind == LN_ValueExpressionKind::UIElementEventObject) {
      target_expression_index = index;
    }
    if (expression.kind == LN_ValueExpressionKind::ObjectGameProperty) {
      property_expression_index = index;
    }
  }
  ASSERT_NE(target_expression_index, LN_INVALID_INDEX);
  ASSERT_NE(property_expression_index, LN_INVALID_INDEX);
  const LN_ValueExpression &property_expression =
      program->GetValueExpressions()[property_expression_index];
  EXPECT_EQ(property_expression.input0, target_expression_index);
  const LN_StringExpression *name_expression = StringExpressionAt(
      *program, property_expression.input1);
  ASSERT_NE(name_expression, nullptr);
  EXPECT_EQ(name_expression->kind, LN_StringExpressionKind::Constant);
  EXPECT_EQ(name_expression->string_value, "exit");
  EXPECT_TRUE(program->GetGamePropertyRefs().empty());

  const LN_Instruction *route = FindBranchRouteWithConditionKind(
      *program,
      program->GetInstructions(LN_Event::OnUpdate),
      LN_BoolExpressionKind::ValueIsNone);
  ASSERT_NE(route, nullptr);
  EXPECT_TRUE(BoolExpressionTreeContainsKind(
      *program, route->bool_expr_index, LN_BoolExpressionKind::ValueIsNone));
  EXPECT_TRUE(BoolExpressionTreeContainsKind(
      *program, route->bool_expr_index, LN_BoolExpressionKind::Not));
  EXPECT_EQ(program->GetMainThreadOnlyReason(), LN_MainThreadOnlyReason::QueryExpression);

  /* Leaving Object empty retains the existing tree-owner snapshot fast path. */
  Remove(tree.links, object_to_property);
  const std::shared_ptr<LN_Program> owner_program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(owner_program, nullptr);
  EXPECT_FALSE(owner_program->GetCompileReport().HasErrors())
      << FormatCompileReport(owner_program->GetCompileReport());
  ASSERT_TRUE(owner_program->ValidateInstructionPayloads());
  EXPECT_TRUE(HasGamePropertyRef(*owner_program, "exit", LN_ValueType::None));
  EXPECT_TRUE(std::none_of(owner_program->GetValueExpressions().begin(),
                           owner_program->GetValueExpressions().end(),
                           [](const LN_ValueExpression &expression) {
                             return expression.kind ==
                                    LN_ValueExpressionKind::ObjectGameProperty;
                           }));

  /* A blank name is always False: the rest of the tree must still compile. */
  SetCString(property_name_default.value, "");
  for (const bool object_linked : {false, true}) {
    if (object_linked) {
      Append(tree.links, object_to_property);
    }
    const std::shared_ptr<LN_Program> blank_program = LN_TreeCompiler().Compile(tree);
    ASSERT_NE(blank_program, nullptr);
    EXPECT_FALSE(blank_program->GetCompileReport().HasErrors())
        << FormatCompileReport(blank_program->GetCompileReport());
    EXPECT_TRUE(HasWarningContaining(blank_program->GetCompileReport(), "always False"));
    ASSERT_TRUE(blank_program->ValidateInstructionPayloads());
    EXPECT_TRUE(blank_program->GetGamePropertyRefs().empty());
    EXPECT_EQ(FindBranchRouteWithConditionKind(*blank_program,
                                               blank_program->GetInstructions(
                                                   LN_Event::OnFixedUpdate),
                                               LN_BoolExpressionKind::ValueIsNone),
              nullptr);
  }
}

/* A slot let go over the world: Drop Item takes the event's item and point straight from a UI
 * route; Logic + Physics timing is its own business. */
TEST(LN_TreeCompiler, AUIEventDropsItsSlotIntoTheWorldWithDropItem)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNodeSocketValueString action_default = {};
  SetCString(action_default.value, "slot");
  blender::bNode on_ui_event = {};
  blender::bNodeSocket action_input = {};
  std::array<blender::bNodeSocket, 10> event_outputs = {};
  InitNode(on_ui_event, "LogicNativeOnUIEvent", "On UI Event", 1);
  on_ui_event.custom1 = int16_t(LN_UIElementEvent::DropOutside);
  InitSocket(action_input, "Action", blender::SOCK_STRING, &action_default);
  Append(on_ui_event.inputs, action_input);
  const std::array<const char *, 10> event_names = {
      "Out", "Object", "Action", "Target", "Point", "Item", "Index", "Target Item",
      "Target Index", "User"};
  const std::array<blender::eNodeSocketDatatype, 10> event_types = {
      blender::SOCK_BOOLEAN, blender::SOCK_OBJECT, blender::SOCK_STRING, blender::SOCK_OBJECT,
      blender::SOCK_VECTOR,  blender::SOCK_CUSTOM, blender::SOCK_INT,    blender::SOCK_CUSTOM,
      blender::SOCK_INT,     blender::SOCK_INT};
  for (size_t index = 0; index < event_outputs.size(); ++index) {
    InitSocket(event_outputs[index],
               event_names[index],
               event_types[index],
               nullptr,
               index == 0 ? "NodeSocketLogicExecution" : nullptr);
    Append(on_ui_event.outputs, event_outputs[index]);
  }
  Append(tree.nodes, on_ui_event);

  blender::bNodeSocketValueString empty_item = {};
  blender::bNodeSocketValueInt zero = {};
  blender::bNodeSocketValueVector origin = {};
  blender::bNodeSocketValueCollection no_library = {};
  blender::bNode drop = {};
  std::array<blender::bNodeSocket, 5> drop_inputs = {};
  blender::bNodeSocket drop_done = {};
  InitNode(drop, "LogicNativeDropItem", "Drop Item", 2);
  InitSocket(drop_inputs[0], "Flow", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  InitSocket(drop_inputs[1], "Item", blender::SOCK_STRING, &empty_item);
  InitSocket(drop_inputs[2], "Amount", blender::SOCK_INT, &zero);
  InitSocket(drop_inputs[3], "Position", blender::SOCK_VECTOR, &origin);
  InitSocket(drop_inputs[4], "Library", blender::SOCK_COLLECTION, &no_library);
  InitSocket(drop_done, "Done", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  for (blender::bNodeSocket &socket : drop_inputs) {
    Append(drop.inputs, socket);
  }
  Append(drop.outputs, drop_done);
  Append(tree.nodes, drop);

  blender::bNode print_node = {};
  blender::bNodeSocket print_flow = {};
  blender::bNodeSocket print_value = {};
  InitNode(print_node, "LogicNativePrint", "Print", 3);
  InitSocket(print_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(print_value, "Value", blender::SOCK_CUSTOM, nullptr);
  Append(print_node.inputs, print_flow);
  Append(print_node.inputs, print_value);
  Append(tree.nodes, print_node);

  std::array<blender::bNodeLink, 4> links = {};
  InitLink(links[0], on_ui_event, event_outputs[0], drop, drop_inputs[0]);
  InitLink(links[1], on_ui_event, event_outputs[5], drop, drop_inputs[1]);
  InitLink(links[2], on_ui_event, event_outputs[4], drop, drop_inputs[3]);
  InitLink(links[3], drop, drop_done, print_node, print_flow);
  for (blender::bNodeLink &link : links) {
    Append(tree.links, link);
  }

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(program, nullptr);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  const LN_Instruction *drop_instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnUpdate), LN_OpCode::DropItem);
  ASSERT_NE(drop_instruction, nullptr);
  EXPECT_NE(drop_instruction->flags & LN_INSTRUCTION_FLAG_UI_EVENT_ROUTE, 0);
  const LN_ValueExpression *item = ValueExpressionAt(*program,
                                                     drop_instruction->secondary_value_expr_index);
  ASSERT_NE(item, nullptr);
  EXPECT_EQ(item->kind, LN_ValueExpressionKind::UIElementEventItem);
  const LN_VectorExpression *position = VectorExpressionAt(*program,
                                                           drop_instruction->vector_expr_index);
  ASSERT_NE(position, nullptr);
  EXPECT_EQ(position->kind, LN_VectorExpressionKind::PhysicsQueryPoint);
  /* No Library: the scene's Item Library at runtime. */
  EXPECT_EQ(drop_instruction->value_expr_index, LN_INVALID_INDEX);

  /* Done runs only when the drop ran: its route tests the drop instruction. */
  const std::vector<LN_Instruction> &instructions = program->GetInstructions(LN_Event::OnUpdate);
  const LN_Instruction *done_route = FindBranchRouteWithConditionKind(
      *program, instructions, LN_BoolExpressionKind::InstructionExecuted);
  ASSERT_NE(done_route, nullptr);
  const LN_BoolExpression *dropped = BoolExpressionAt(*program, done_route->bool_expr_index);
  ASSERT_NE(dropped, nullptr);
  EXPECT_EQ(dropped->kind, LN_BoolExpressionKind::InstructionExecuted);
  EXPECT_EQ(dropped->input0, uint32_t(drop_instruction - instructions.data()));
  const LN_Instruction *print = FindInstruction(instructions, LN_OpCode::Print);
  ASSERT_NE(print, nullptr);
  ExpectInstructionGuardedByRouteCondition(
      *program, LN_Event::OnUpdate, *print, LN_BoolExpressionKind::InstructionExecuted);
}

/* String Operation's Result also reads text known only at runtime, such as a Choice property. */
TEST(LN_TreeCompiler, StringOperationResultReadsRuntimeText)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueObject no_object = {};
  blender::bNodeSocketValueString property_default = {};
  SetCString(property_default.value, "sort_by");
  blender::bNode get_property = {};
  std::array<blender::bNodeSocket, 2> get_inputs = {};
  blender::bNodeSocket get_value = {};
  InitNode(get_property, "LogicNativeGetGameProperty", "Get Object Property", 2);
  InitSocket(get_inputs[0], "Object", blender::SOCK_OBJECT, &no_object);
  InitSocket(get_inputs[1], "Property", blender::SOCK_STRING, &property_default);
  InitSocket(get_value, "Value", blender::SOCK_CUSTOM, nullptr);
  for (blender::bNodeSocket &socket : get_inputs) {
    Append(get_property.inputs, socket);
  }
  Append(get_property.outputs, get_value);
  Append(tree.nodes, get_property);

  blender::bNodeSocketValueString empty_string = {};
  blender::bNodeSocketValueString count_default = {};
  SetCString(count_default.value, "Count");
  blender::bNodeSocketValueInt zero = {};
  blender::bNode operation = {};
  std::array<blender::bNodeSocket, 4> operation_inputs = {};
  std::array<blender::bNodeSocket, 3> operation_outputs = {};
  InitNode(operation, "LogicNativeStringOperation", "String Operation", 3);
  operation.custom1 = 2; /* Contains. */
  InitSocket(operation_inputs[0], "String", blender::SOCK_STRING, &empty_string);
  InitSocket(operation_inputs[1], "Substring", blender::SOCK_STRING, &count_default);
  InitSocket(operation_inputs[2], "Replacement", blender::SOCK_STRING, &empty_string);
  InitSocket(operation_inputs[3], "Length", blender::SOCK_INT, &zero);
  InitSocket(operation_outputs[0], "String", blender::SOCK_STRING, nullptr);
  InitSocket(operation_outputs[1], "Result", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(operation_outputs[2], "Count", blender::SOCK_INT, nullptr);
  for (blender::bNodeSocket &socket : operation_inputs) {
    Append(operation.inputs, socket);
  }
  for (blender::bNodeSocket &socket : operation_outputs) {
    Append(operation.outputs, socket);
  }
  Append(tree.nodes, operation);

  blender::bNodeSocketValueBoolean false_default = {};
  blender::bNode branch = {};
  blender::bNodeSocket branch_flow = {};
  blender::bNodeSocket branch_condition = {};
  blender::bNodeSocket branch_true = {};
  blender::bNodeSocket branch_false = {};
  InitNode(branch, "LogicNativeBranch", "Branch", 4);
  InitSocket(branch_flow, "Flow", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  InitSocket(branch_condition, "Condition", blender::SOCK_BOOLEAN, &false_default);
  InitSocket(branch_true, "True", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  InitSocket(branch_false, "False", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  Append(branch.inputs, branch_flow);
  Append(branch.inputs, branch_condition);
  Append(branch.outputs, branch_true);
  Append(branch.outputs, branch_false);
  Append(tree.nodes, branch);

  blender::bNode print_node = {};
  blender::bNodeSocket print_flow = {};
  blender::bNodeSocket print_value = {};
  InitNode(print_node, "LogicNativePrint", "Print", 5);
  InitSocket(print_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(print_value, "Value", blender::SOCK_CUSTOM, nullptr);
  Append(print_node.inputs, print_flow);
  Append(print_node.inputs, print_value);
  Append(tree.nodes, print_node);

  std::array<blender::bNodeLink, 4> links = {};
  InitLink(links[0], on_update, on_update_pulse, branch, branch_flow);
  InitLink(links[1], get_property, get_value, operation, operation_inputs[0]);
  InitLink(links[2], operation, operation_outputs[1], branch, branch_condition);
  InitLink(links[3], branch, branch_true, print_node, print_flow);
  for (blender::bNodeLink &link : links) {
    Append(tree.links, link);
  }

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(program, nullptr);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  std::vector<std::string> validation_errors;
  EXPECT_TRUE(program->ValidateInstructionPayloads(&validation_errors))
      << (validation_errors.empty() ? std::string() : validation_errors.front());
  const LN_Instruction *route = FindInstruction(program->GetInstructions(LN_Event::OnUpdate),
                                                LN_OpCode::BranchRoute);
  ASSERT_NE(route, nullptr);
  const LN_BoolExpression *contains = BoolExpressionAt(*program, route->bool_expr_index);
  ASSERT_NE(contains, nullptr);
  EXPECT_EQ(contains->kind, LN_BoolExpressionKind::StringContains);
  EXPECT_NE(StringExpressionAt(*program, contains->input0), nullptr);
  const LN_StringExpression *part = StringExpressionAt(*program, contains->input1);
  ASSERT_NE(part, nullptr);
  EXPECT_EQ(part->string_value, "Count");
}

/* Drop Received names both slots: Move To Slot takes them straight from the event. */
TEST(LN_TreeCompiler, MoveToSlotTakesBothSlotsFromDropReceived)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNodeSocketValueString action_default = {};
  SetCString(action_default.value, "slot");
  blender::bNode on_ui_event = {};
  blender::bNodeSocket action_input = {};
  std::array<blender::bNodeSocket, 10> event_outputs = {};
  InitNode(on_ui_event, "LogicNativeOnUIEvent", "On UI Event", 1);
  on_ui_event.custom1 = int16_t(LN_UIElementEvent::DropReceived);
  InitSocket(action_input, "Action", blender::SOCK_STRING, &action_default);
  Append(on_ui_event.inputs, action_input);
  const std::array<const char *, 10> event_names = {
      "Out", "Object", "Action", "Target", "Point", "Item", "Index", "Target Item",
      "Target Index", "User"};
  const std::array<blender::eNodeSocketDatatype, 10> event_types = {
      blender::SOCK_BOOLEAN, blender::SOCK_OBJECT, blender::SOCK_STRING, blender::SOCK_OBJECT,
      blender::SOCK_VECTOR,  blender::SOCK_CUSTOM, blender::SOCK_INT,    blender::SOCK_CUSTOM,
      blender::SOCK_INT,     blender::SOCK_INT};
  for (size_t index = 0; index < event_outputs.size(); ++index) {
    InitSocket(event_outputs[index],
               event_names[index],
               event_types[index],
               nullptr,
               index == 0 ? "NodeSocketLogicExecution" : nullptr);
    Append(on_ui_event.outputs, event_outputs[index]);
  }
  Append(tree.nodes, on_ui_event);

  blender::bNodeSocketValueString property_default = {};
  SetCString(property_default.value, "inventory");
  blender::bNodeSocketValueString empty_string = {};
  blender::bNodeSocketValueInt int_default = {};
  blender::bNodeSocketValueBoolean true_default = {};
  true_default.value = true;
  blender::bNodeSocketValueCollection no_library = {};
  blender::bNode inventory = {};
  blender::bNodeSocketValueBoolean keep_places_default = {};
  InitNode(inventory, "LogicNativeInventory", "Move To Slot", 2);
  inventory.custom1 = int16_t(LN_InventoryOperation::MoveToSlot);
  const std::array<const char *, 20> input_names = {"Flow",
                                                    "Object",
                                                    "Property",
                                                    "Item",
                                                    "Amount",
                                                    "Max Stack",
                                                    "Stack",
                                                    "Capacity",
                                                    "Slot",
                                                    "Other Slot",
                                                    "By Count",
                                                    "Descending",
                                                    "Recipes",
                                                    "Recipe",
                                                    "Library",
                                                    "Swap",
                                                    "To Object",
                                                    "To Property",
                                                    "Keep Places",
                                                    "Override"};
  const std::array<blender::eNodeSocketDatatype, 20> input_types = {
      blender::SOCK_BOOLEAN, blender::SOCK_OBJECT,  blender::SOCK_STRING,     blender::SOCK_STRING,
      blender::SOCK_INT,     blender::SOCK_INT,     blender::SOCK_BOOLEAN,    blender::SOCK_INT,
      blender::SOCK_INT,     blender::SOCK_INT,     blender::SOCK_BOOLEAN,    blender::SOCK_BOOLEAN,
      blender::SOCK_CUSTOM,  blender::SOCK_STRING,  blender::SOCK_COLLECTION, blender::SOCK_BOOLEAN,
      blender::SOCK_OBJECT,  blender::SOCK_STRING,  blender::SOCK_BOOLEAN,    blender::SOCK_BOOLEAN};
  const std::array<void *, 20> input_defaults = {nullptr,
                                                 nullptr,
                                                 &property_default,
                                                 &empty_string,
                                                 &int_default,
                                                 &int_default,
                                                 &true_default,
                                                 &int_default,
                                                 &int_default,
                                                 &int_default,
                                                 &true_default,
                                                 &true_default,
                                                 nullptr,
                                                 &empty_string,
                                                 &no_library,
                                                 &true_default,
                                                 nullptr,
                                                 &empty_string,
                                                 &keep_places_default,
                                                 &keep_places_default};
  std::array<blender::bNodeSocket, 20> inputs = {};
  for (size_t index = 0; index < inputs.size(); ++index) {
    InitSocket(inputs[index],
               input_names[index],
               input_types[index],
               input_defaults[index],
               index == 0 ? "NodeSocketLogicExecution" :
                            (index == 12 ? "NodeSocketLogicList" : nullptr));
    Append(inventory.inputs, inputs[index]);
  }
  blender::bNodeSocket done = {};
  InitSocket(done, "Done", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  Append(inventory.outputs, done);
  Append(tree.nodes, inventory);

  std::array<blender::bNodeLink, 3> links = {};
  InitLink(links[0], on_ui_event, event_outputs[0], inventory, inputs[0]);
  /* The dropped slot moves onto the slot it was dropped on. */
  InitLink(links[1], on_ui_event, event_outputs[8], inventory, inputs[8]);
  InitLink(links[2], on_ui_event, event_outputs[6], inventory, inputs[9]);
  for (blender::bNodeLink &link : links) {
    Append(tree.links, link);
  }

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(program, nullptr);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  const LN_Instruction *edit_instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnUpdate), LN_OpCode::InventoryEdit);
  ASSERT_NE(edit_instruction, nullptr);
  const LN_ValueExpression *edit = ValueExpressionAt(*program,
                                                     edit_instruction->secondary_value_expr_index);
  ASSERT_NE(edit, nullptr);
  /* Move To Slot is not the zero default (Add Item), so this shows the operation compiled. */
  EXPECT_EQ(edit->value.int_value, int32_t(LN_InventoryOperation::MoveToSlot));
  const auto int_kind = [&](const uint32_t value_index) {
    const LN_ValueExpression *value = ValueExpressionAt(*program, value_index);
    if (value == nullptr) {
      return LN_IntExpressionKind::Count;
    }
    const LN_IntExpression *source = value->kind == LN_ValueExpressionKind::FromInt ?
                                         IntExpressionAt(*program, value->input0) :
                                         nullptr;
    return source != nullptr ? source->kind : LN_IntExpressionKind::Count;
  };
  EXPECT_EQ(int_kind(edit->input_indices[LN_INVENTORY_INPUT_SLOT]),
            LN_IntExpressionKind::UIElementEventTargetIndex);
  EXPECT_EQ(int_kind(edit->input_indices[LN_INVENTORY_INPUT_OTHER_SLOT]),
            LN_IntExpressionKind::UIElementEventIndex);
}

TEST(LN_TreeCompiler, OnUIEventCompilesAFrameRouteWithActionFilterAndElementOutputs)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNodeSocketValueString action_default = {};
  blender::bNodeSocketValueObject object_default = {};
  blender::bNodeSocketValueBoolean false_default = {};
  SetCString(action_default.value, "play");

  blender::bNode on_ui_event = {};
  blender::bNodeSocket action_input = {};
  blender::bNodeSocket out = {};
  blender::bNodeSocket element = {};
  blender::bNodeSocket action_output = {};
  blender::bNodeSocket target = {};
  blender::bNodeSocket point = {};
  InitNode(on_ui_event, "LogicNativeOnUIEvent", "On UI Event", 1);
  on_ui_event.custom1 = int16_t(LN_UIElementEvent::Click);
  InitSocket(action_input, "Action", blender::SOCK_STRING, &action_default);
  InitSocket(out, "Out", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  InitSocket(element, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(action_output, "Action", blender::SOCK_STRING, nullptr);
  InitSocket(target, "Target", blender::SOCK_OBJECT, nullptr);
  InitSocket(point, "Point", blender::SOCK_VECTOR, nullptr);
  Append(on_ui_event.inputs, action_input);
  Append(on_ui_event.outputs, out);
  Append(on_ui_event.outputs, element);
  Append(on_ui_event.outputs, action_output);
  Append(on_ui_event.outputs, target);
  Append(on_ui_event.outputs, point);
  Append(tree.nodes, on_ui_event);

  blender::bNode set_interactable = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket set_object = {};
  blender::bNodeSocket interactable = {};
  blender::bNodeSocket done = {};
  InitNode(set_interactable, "LogicNativeSetUIInteractable", "Set UI Interactable", 2);
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  InitSocket(set_object, "Object", blender::SOCK_OBJECT, &object_default);
  InitSocket(interactable, "Interactable", blender::SOCK_BOOLEAN, &false_default);
  InitSocket(done, "Done", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  Append(set_interactable.inputs, flow);
  Append(set_interactable.inputs, set_object);
  Append(set_interactable.inputs, interactable);
  Append(set_interactable.outputs, done);
  Append(tree.nodes, set_interactable);

  blender::bNodeLink out_to_flow = {};
  blender::bNodeLink element_to_object = {};
  InitLink(out_to_flow, on_ui_event, out, set_interactable, flow);
  InitLink(element_to_object, on_ui_event, element, set_interactable, set_object);
  Append(tree.links, out_to_flow);
  Append(tree.links, element_to_object);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(program, nullptr);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  /* On UI Event is an event root: a UI-only tree gets no missing-root hint. */
  EXPECT_FALSE(HasWarningContaining(program->GetCompileReport(), "has no On Init"));

  uint32_t query_index = LN_INVALID_INDEX;
  for (uint32_t index = 0; index < program->GetQueryExpressions().size(); ++index) {
    if (program->GetQueryExpressions()[index].kind == LN_QueryExpressionKind::UIElementEvent) {
      query_index = index;
    }
  }
  ASSERT_NE(query_index, LN_INVALID_INDEX);
  const LN_QueryExpression &query = program->GetQueryExpressions()[query_index];
  EXPECT_EQ(query.int_value, int32_t(LN_UIElementEvent::Click));
  const LN_StringExpression *action = StringExpressionAt(*program, query.string_expr_index);
  ASSERT_NE(action, nullptr);
  EXPECT_EQ(action->string_value, "play");

  /* UI events pulse once per rendered frame, never on Logic + Physics steps. */
  const LN_Instruction *update_command = FindInstruction(
      program->GetInstructions(LN_Event::OnUpdate), LN_OpCode::SetUIInteractable);
  ASSERT_NE(update_command, nullptr);
  /* A route a UI event starts keeps running while the game is paused. */
  EXPECT_NE(update_command->flags & LN_INSTRUCTION_FLAG_UI_EVENT_ROUTE, 0);
  EXPECT_TRUE(program->HasUIEventRoutes());
  EXPECT_EQ(FindInstruction(program->GetInstructions(LN_Event::OnFixedUpdate),
                            LN_OpCode::SetUIInteractable),
            nullptr);
  EXPECT_NE(FindBranchRouteWithConditionKind(*program,
                                             program->GetInstructions(LN_Event::OnUpdate),
                                             LN_BoolExpressionKind::UIElementEventFired),
            nullptr);
  const LN_ValueExpression *target_object = ValueExpressionAt(*program,
                                                              update_command->value_expr_index);
  ASSERT_NE(target_object, nullptr);
  EXPECT_EQ(target_object->kind, LN_ValueExpressionKind::UIElementEventObject);
  EXPECT_EQ(target_object->input0, query_index);
}

TEST(LN_TreeCompiler, ObjectViewsCompileAsAutonomousQueriesWithoutOutputConsumers)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::Object content_object = {};
  SetCString(content_object.id.name, "OBScrollContent");
  blender::bNodeSocketValueObject object_default = {};
  blender::bNodeSocketValueObject content_default = {};
  content_default.value = &content_object;
  blender::bNodeSocketValueVector zero_vector = {};
  blender::bNodeSocketValueBoolean true_default = {};
  blender::bNodeSocketValueBoolean false_default = {};
  blender::bNodeSocketValueInt zero_int = {};
  blender::bNodeSocketValueInt one_int = {};
  blender::bNodeSocketValueFloat zero_float = {};
  true_default.value = true;
  one_int.value = 1;

  blender::bNode scroll = {};
  InitNode(scroll, "LogicNativeObjectScrollView", "Object Scroll View", 50);
  const std::array<const char *, 14> scroll_names = {"Viewport Object",
                                                     "Content Object",
                                                     "Camera",
                                                     "Minimum Offset",
                                                     "Maximum Offset",
                                                     "Wheel Step",
                                                     "Enabled",
                                                     "Horizontal",
                                                     "Vertical",
                                                     "Cull Outside",
                                                     "Focus Follow",
                                                     "User",
                                                     "Priority",
                                                     "Padding"};
  const std::array<blender::eNodeSocketDatatype, 14> scroll_types = {
      blender::SOCK_OBJECT, blender::SOCK_OBJECT,  blender::SOCK_OBJECT,
      blender::SOCK_VECTOR, blender::SOCK_VECTOR,  blender::SOCK_VECTOR,
      blender::SOCK_BOOLEAN, blender::SOCK_BOOLEAN, blender::SOCK_BOOLEAN,
      blender::SOCK_BOOLEAN, blender::SOCK_BOOLEAN, blender::SOCK_INT,
      blender::SOCK_INT,     blender::SOCK_FLOAT};
  const std::array<void *, 14> scroll_defaults = {&object_default,
                                                  &content_default,
                                                  &object_default,
                                                  &zero_vector,
                                                  &zero_vector,
                                                  &zero_vector,
                                                  &true_default,
                                                  &false_default,
                                                  &true_default,
                                                  &true_default,
                                                  &true_default,
                                                  &zero_int,
                                                  &zero_int,
                                                  &zero_float};
  std::array<blender::bNodeSocket, 14> scroll_inputs = {};
  for (size_t index = 0; index < scroll_inputs.size(); ++index) {
    InitSocket(scroll_inputs[index],
               scroll_names[index],
               scroll_types[index],
               scroll_defaults[index]);
    Append(scroll.inputs, scroll_inputs[index]);
  }
  Append(tree.nodes, scroll);

  blender::Object template_object = {};
  SetCString(template_object.id.name, "OBSlot");
  blender::bNodeSocketValueObject template_default = {};
  template_default.value = &template_object;
  blender::bNodeSocketValueInt rows_default = {};
  rows_default.value = 3;
  blender::Object bar_object = {};
  SetCString(bar_object.id.name, "OBBar");
  blender::bNodeSocketValueObject bar_default = {};
  bar_default.value = &bar_object;
  blender::bNode grid = {};
  InitNode(grid, "LogicNativeObjectGridView", "Object Grid View", 51);
  const std::array<const char *, 20> grid_names = {"Viewport Object",
                                                  "Camera",
                                                  "Slots",
                                                  "Items",
                                                  "First Item",
                                                  "Columns",
                                                  "Wheel Rows",
                                                  "Enabled",
                                                  "Cull Outside",
                                                  "Focus Follow",
                                                  "User",
                                                  "Priority",
                                                  "Padding",
                                                  "Show Counts",
                                                  "Show Names",
                                                  "Show Empty",
                                                  "Slot Template",
                                                  "Rows",
                                                  "Spacing",
                                                  "Scrollbar"};
  const std::array<blender::eNodeSocketDatatype, 20> grid_types = {
      blender::SOCK_OBJECT,  blender::SOCK_OBJECT,  blender::SOCK_CUSTOM,
      blender::SOCK_CUSTOM,  blender::SOCK_INT,     blender::SOCK_INT,
      blender::SOCK_INT,     blender::SOCK_BOOLEAN, blender::SOCK_BOOLEAN,
      blender::SOCK_BOOLEAN, blender::SOCK_INT,     blender::SOCK_INT,
      blender::SOCK_FLOAT,   blender::SOCK_BOOLEAN, blender::SOCK_BOOLEAN,
      blender::SOCK_BOOLEAN, blender::SOCK_OBJECT,  blender::SOCK_INT,
      blender::SOCK_VECTOR,  blender::SOCK_OBJECT};
  const std::array<void *, 20> grid_defaults = {&object_default,
                                                &object_default,
                                                nullptr,
                                                nullptr,
                                                &zero_int,
                                                &one_int,
                                                &one_int,
                                                &true_default,
                                                &true_default,
                                                &true_default,
                                                &zero_int,
                                                &zero_int,
                                                &zero_float,
                                                &true_default,
                                                &true_default,
                                                &true_default,
                                                &template_default,
                                                &rows_default,
                                                &zero_vector,
                                                &bar_default};
  std::array<blender::bNodeSocket, 20> grid_inputs = {};
  for (size_t index = 0; index < grid_inputs.size(); ++index) {
    InitSocket(grid_inputs[index],
               grid_names[index],
               grid_types[index],
               grid_defaults[index],
               grid_types[index] == blender::SOCK_CUSTOM ? "NodeSocketLogicGeneric" : nullptr);
    Append(grid.inputs, grid_inputs[index]);
  }
  GridListSockets grid_list;
  grid_list.AppendTo(grid);
  Append(tree.nodes, grid);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(program, nullptr);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  std::vector<std::string> validation_errors;
  ASSERT_TRUE(program->ValidateInstructionPayloads(&validation_errors))
      << (validation_errors.empty() ? std::string() : validation_errors.front());
  ASSERT_EQ(program->GetQueryExpressions().size(), 2u);
  const auto find_query = [&](const LN_QueryExpressionKind kind) {
    return std::find_if(program->GetQueryExpressions().begin(),
                        program->GetQueryExpressions().end(),
                        [&](const LN_QueryExpression &query) { return query.kind == kind; });
  };
  const auto scroll_query = find_query(LN_QueryExpressionKind::ObjectScrollView);
  const auto grid_query = find_query(LN_QueryExpressionKind::ObjectGridView);
  ASSERT_NE(scroll_query, program->GetQueryExpressions().end());
  ASSERT_NE(grid_query, program->GetQueryExpressions().end());
  EXPECT_NE(scroll_query->input1, LN_INVALID_INDEX);
  EXPECT_EQ(scroll_query->input2, LN_INVALID_INDEX);
  EXPECT_EQ(grid_query->input1, LN_INVALID_INDEX);
  EXPECT_NE(grid_query->senary_bool_expr_index, LN_INVALID_INDEX);
  /* Slot Template, Spacing, Show Counts and Rows reach the view; the icons come from the list's
   * Inventory Rules, not the grid (no Icon Library input). */
  EXPECT_EQ(grid_query->input6, LN_INVALID_INDEX);
  const LN_ValueExpression *slot_template = ValueExpressionAt(*program, grid_query->input7);
  ASSERT_NE(slot_template, nullptr);
  EXPECT_EQ(slot_template->value.type, LN_ValueType::ObjectRef);
  EXPECT_NE(grid_query->input8, LN_INVALID_INDEX);
  const LN_ValueExpression *scrollbar = ValueExpressionAt(*program, grid_query->input9);
  ASSERT_NE(scrollbar, nullptr);
  EXPECT_EQ(scrollbar->value.type, LN_ValueType::ObjectRef);
  EXPECT_EQ(scroll_query->input9, LN_INVALID_INDEX);
  EXPECT_NE(grid_query->quaternary_bool_expr_index, LN_INVALID_INDEX);
  /* Show Names. */
  EXPECT_NE(grid_query->quinary_bool_expr_index, LN_INVALID_INDEX);
  const LN_IntExpression *rows = IntExpressionAt(*program, grid_query->quaternary_int_expr_index);
  ASSERT_NE(rows, nullptr);
  EXPECT_EQ(rows->int_value, 3);
  EXPECT_TRUE(program->GetInstructions(LN_Event::OnUpdate).empty());
}

TEST(LN_TreeCompiler, ObjectMenuSessionFocusAndAnchorCommandsCompileWithDeferredResults)
{
  blender::bNodeTree tree = {};
  InitTree(tree);
  blender::Object camera_object = {};
  blender::Object target_object = {};
  SetCString(camera_object.id.name, "OBCamera");
  SetCString(target_object.id.name, "OBButton");
  camera_object.type = blender::OB_CAMERA;
  blender::Collection collection = {};
  SetCString(collection.id.name, "COInventory");

  blender::bNodeSocketValueObject camera_default = {};
  blender::bNodeSocketValueObject target_default = {};
  blender::bNodeSocketValueCollection collection_default = {};
  blender::bNodeSocketValueString context_default = {};
  blender::bNodeSocketValueInt user_default = {};
  blender::bNodeSocketValueFloat depth_default = {};
  blender::bNodeSocketValueVector zero_vector = {};
  blender::bNodeSocketValueVector reference_resolution = {};
  blender::bNodeSocketValueBoolean scale_default = {};
  camera_default.value = &camera_object;
  target_default.value = &target_object;
  collection_default.value = &collection;
  SetCString(context_default.value, "Inventory");
  depth_default.value = 2.0f;
  reference_resolution.value[0] = 1920.0f;
  reference_resolution.value[1] = 1080.0f;
  scale_default.value = true;

  blender::bNode on_fixed = {};
  blender::bNodeSocket fixed_out = {};
  InitNode(on_fixed, "LogicNativeOnFixedUpdate", "On Fixed Update", 1);
  InitSocket(fixed_out, "Out", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  Append(on_fixed.outputs, fixed_out);
  Append(tree.nodes, on_fixed);

  blender::bNode open = {};
  std::array<blender::bNodeSocket, 5> open_inputs = {};
  std::array<blender::bNodeSocket, 4> open_outputs = {};
  blender::bNodeSocketValueInt open_user_default = {};
  open_user_default.value = 2;
  InitNode(open, "LogicNativeOpenObjectMenu", "Open Object Menu", 2);
  /* No setting is the zero default, so each one shows it was compiled. */
  open.custom1 = int32_t(LN_UIMenuRouting::PassThrough);
  open.custom2 = LN_PackUIMenuNodeSettings(
      LN_UIMenuOpenMode::ReplaceTop, LN_UIMenuPresentation::Overlay, LN_UIMenuBackBehavior::Ignore);
  InitSocket(open_inputs[0], "Flow", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  InitSocket(open_inputs[1], "Camera", blender::SOCK_OBJECT, &camera_default);
  InitSocket(open_inputs[2], "Collection", blender::SOCK_COLLECTION, &collection_default);
  InitSocket(open_inputs[3], "Input Context", blender::SOCK_STRING, &context_default);
  InitSocket(open_inputs[4], "User", blender::SOCK_INT, &open_user_default);
  InitSocket(open_outputs[0], "Opened", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  InitSocket(open_outputs[1], "Failed", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  InitSocket(open_outputs[2], "Menu", blender::SOCK_INT, nullptr);
  InitSocket(open_outputs[3], "Result", blender::SOCK_INT, nullptr);
  for (blender::bNodeSocket &socket : open_inputs) {
    Append(open.inputs, socket);
  }
  for (blender::bNodeSocket &socket : open_outputs) {
    Append(open.outputs, socket);
  }
  Append(tree.nodes, open);

  blender::bNode close = {};
  blender::bNodeSocket close_flow = {};
  blender::bNodeSocket close_handle = {};
  blender::bNodeSocket close_done = {};
  blender::bNodeSocket close_user = {};
  blender::bNodeSocketValueInt close_user_default = {};
  close_user_default.value = 3;
  InitNode(close, "LogicNativeCloseObjectMenu", "Close Object Menu", 3);
  close.custom1 = int32_t(LN_UIMenuCloseOperation::Specific);
  InitSocket(close_flow, "Flow", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  InitSocket(close_handle, "Menu", blender::SOCK_INT, &user_default);
  InitSocket(close_user, "User", blender::SOCK_INT, &close_user_default);
  InitSocket(close_done, "Closed", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  Append(close.inputs, close_flow);
  Append(close.inputs, close_handle);
  Append(close.inputs, close_user);
  Append(close.outputs, close_done);
  Append(tree.nodes, close);

  blender::bNode set_focus = {};
  std::array<blender::bNodeSocket, 4> focus_inputs = {};
  InitNode(set_focus, "LogicNativeSetUIFocus", "Set UI Focus", 4);
  InitSocket(focus_inputs[0], "Flow", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  InitSocket(focus_inputs[1], "Object", blender::SOCK_OBJECT, &target_default);
  InitSocket(focus_inputs[2], "User", blender::SOCK_INT, &user_default);
  InitSocket(focus_inputs[3], "Input Context", blender::SOCK_STRING, &context_default);
  for (blender::bNodeSocket &socket : focus_inputs) {
    Append(set_focus.inputs, socket);
  }
  Append(tree.nodes, set_focus);

  blender::bNode anchor = {};
  std::array<blender::bNodeSocket, 9> anchor_inputs = {};
  blender::bNodeSocketValueInt anchor_user = {};
  InitNode(anchor, "LogicNativeScreenAnchor", "Screen Anchor", 5);
  anchor.custom1 = 8;
  InitSocket(anchor_inputs[0], "Flow", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  InitSocket(anchor_inputs[1], "Object", blender::SOCK_OBJECT, &target_default);
  InitSocket(anchor_inputs[2], "Camera", blender::SOCK_OBJECT, &camera_default);
  InitSocket(anchor_inputs[3], "Offset Pixels", blender::SOCK_VECTOR, &zero_vector);
  InitSocket(anchor_inputs[4], "Safe Margin Pixels", blender::SOCK_VECTOR, &zero_vector);
  InitSocket(anchor_inputs[5], "Depth", blender::SOCK_FLOAT, &depth_default);
  InitSocket(anchor_inputs[6],
             "Reference Resolution",
             blender::SOCK_VECTOR,
             &reference_resolution);
  InitSocket(anchor_inputs[7], "Scale Offset", blender::SOCK_BOOLEAN, &scale_default);
  InitSocket(anchor_inputs[8], "User", blender::SOCK_INT, &anchor_user);
  for (blender::bNodeSocket &socket : anchor_inputs) {
    Append(anchor.inputs, socket);
  }
  Append(tree.nodes, anchor);

  std::array<blender::bNodeLink, 5> links = {};
  InitLink(links[0], on_fixed, fixed_out, open, open_inputs[0]);
  InitLink(links[1], open, open_outputs[0], close, close_flow);
  InitLink(links[2], open, open_outputs[2], close, close_handle);
  InitLink(links[3], on_fixed, fixed_out, set_focus, focus_inputs[0]);
  InitLink(links[4], on_fixed, fixed_out, anchor, anchor_inputs[0]);
  for (blender::bNodeLink &link : links) {
    Append(tree.links, link);
  }

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(program, nullptr);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  const std::vector<LN_Instruction> &instructions = program->GetInstructions(
      LN_Event::OnFixedUpdate);
  const LN_Instruction *open_instruction = FindInstruction(instructions,
                                                           LN_OpCode::OpenObjectMenu);
  const LN_Instruction *close_instruction = FindInstruction(instructions,
                                                            LN_OpCode::CloseObjectMenu);
  const LN_Instruction *focus_instruction = FindInstruction(instructions,
                                                            LN_OpCode::SetUIFocus);
  const LN_Instruction *anchor_instruction = FindInstruction(instructions,
                                                             LN_OpCode::ScreenAnchor);
  ASSERT_NE(open_instruction, nullptr);
  ASSERT_NE(close_instruction, nullptr);
  ASSERT_NE(focus_instruction, nullptr);
  ASSERT_NE(anchor_instruction, nullptr);
  EXPECT_EQ(open_instruction->int_value, int32_t(LN_UIMenuRouting::PassThrough));
  EXPECT_EQ(open_instruction->secondary_int_value,
            LN_PackUIMenuOpenSettings(LN_UIMenuOpenMode::ReplaceTop,
                                     LN_UIMenuPresentation::Overlay,
                                     LN_UIMenuBackBehavior::Ignore));
  EXPECT_EQ(close_instruction->int_value, int32_t(LN_UIMenuCloseOperation::Specific));
  /* Each node's User says whose menus it opens or closes. */
  const LN_IntExpression *open_user = IntExpressionAt(*program, open_instruction->int_expr_index);
  ASSERT_NE(open_user, nullptr);
  EXPECT_EQ(open_user->int_value, 2);
  const LN_IntExpression *close_user_expr = IntExpressionAt(
      *program, close_instruction->secondary_int_expr_index);
  ASSERT_NE(close_user_expr, nullptr);
  EXPECT_EQ(close_user_expr->int_value, 3);
  EXPECT_NE(open_instruction->property_ref_index, LN_INVALID_INDEX);
  EXPECT_NE(close_instruction->property_ref_index, LN_INVALID_INDEX);
  EXPECT_NE(focus_instruction->property_ref_index, LN_INVALID_INDEX);
  EXPECT_EQ(anchor_instruction->int_value, 8);
  const std::array<const LN_Instruction *, 3> completed_commands = {
      open_instruction, close_instruction, focus_instruction};
  EXPECT_TRUE(std::all_of(
      completed_commands.begin(),
      completed_commands.end(),
      [&](const LN_Instruction *instruction) {
        return instruction->property_ref_index < program->GetBoolExpressions().size() &&
               program->GetBoolExpressions()[instruction->property_ref_index].kind ==
                   LN_BoolExpressionKind::UICommandCompleted;
      }));
}

TEST(LN_TreeCompiler, ScreenAnchorWithAnEmptyObjectAnchorsItsOwner)
{
  blender::bNodeTree tree = {};
  InitTree(tree);
  blender::bNodeSocketValueObject empty_object = {};
  blender::bNodeSocketValueFloat depth_default = {};
  blender::bNodeSocketValueVector zero_vector = {};
  blender::bNodeSocketValueVector reference_resolution = {};
  blender::bNodeSocketValueBoolean scale_default = {};
  depth_default.value = 2.0f;
  reference_resolution.value[0] = 1920.0f;
  reference_resolution.value[1] = 1080.0f;

  blender::bNode on_update = {};
  blender::bNodeSocket update_out = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(update_out, "Out", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  Append(on_update.outputs, update_out);
  Append(tree.nodes, on_update);

  blender::bNode anchor = {};
  std::array<blender::bNodeSocket, 9> anchor_inputs = {};
  blender::bNodeSocketValueInt anchor_user = {};
  InitNode(anchor, "LogicNativeScreenAnchor", "Screen Anchor", 2);
  InitSocket(anchor_inputs[0], "Flow", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  InitSocket(anchor_inputs[1], "Object", blender::SOCK_OBJECT, &empty_object);
  InitSocket(anchor_inputs[2], "Camera", blender::SOCK_OBJECT, &empty_object);
  InitSocket(anchor_inputs[3], "Offset Pixels", blender::SOCK_VECTOR, &zero_vector);
  InitSocket(anchor_inputs[4], "Safe Margin Pixels", blender::SOCK_VECTOR, &zero_vector);
  InitSocket(anchor_inputs[5], "Depth", blender::SOCK_FLOAT, &depth_default);
  InitSocket(anchor_inputs[6],
             "Reference Resolution",
             blender::SOCK_VECTOR,
             &reference_resolution);
  InitSocket(anchor_inputs[7], "Scale Offset", blender::SOCK_BOOLEAN, &scale_default);
  InitSocket(anchor_inputs[8], "User", blender::SOCK_INT, &anchor_user);
  for (blender::bNodeSocket &socket : anchor_inputs) {
    Append(anchor.inputs, socket);
  }
  Append(tree.nodes, anchor);

  blender::bNodeLink update_to_anchor = {};
  InitLink(update_to_anchor, on_update, update_out, anchor, anchor_inputs[0]);
  Append(tree.links, update_to_anchor);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(program, nullptr);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  const LN_Instruction *instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnUpdate), LN_OpCode::ScreenAnchor);
  ASSERT_NE(instruction, nullptr);
  /* No Object expression: the runtime anchors the tree's own object. */
  EXPECT_EQ(instruction->value_expr_index, LN_INVALID_INDEX);
  /* On Update is no UI event: this route waits while the game is paused. */
  EXPECT_EQ(instruction->flags & LN_INSTRUCTION_FLAG_UI_EVENT_ROUTE, 0);
  EXPECT_FALSE(program->HasUIEventRoutes());
}

TEST(LN_TreeCompiler, AUIEventRouteContinuesThroughACommandResult)
{
  blender::bNodeTree tree = {};
  InitTree(tree);
  blender::Object camera_object = {};
  SetCString(camera_object.id.name, "OBCamera");
  camera_object.type = blender::OB_CAMERA;
  blender::Collection collection = {};
  SetCString(collection.id.name, "COSettings");
  blender::bNodeSocketValueString action_default = {};
  blender::bNodeSocketValueObject camera_default = {};
  blender::bNodeSocketValueCollection collection_default = {};
  blender::bNodeSocketValueString context_default = {};
  blender::bNodeSocketValueString message_default = {};
  camera_default.value = &camera_object;
  collection_default.value = &collection;
  SetCString(message_default.value, "opened");

  blender::bNode on_ui_event = {};
  blender::bNodeSocket action_input = {};
  blender::bNodeSocket out = {};
  InitNode(on_ui_event, "LogicNativeOnUIEvent", "On UI Event", 1);
  on_ui_event.custom1 = int16_t(LN_UIElementEvent::Click);
  InitSocket(action_input, "Action", blender::SOCK_STRING, &action_default);
  InitSocket(out, "Out", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  Append(on_ui_event.inputs, action_input);
  Append(on_ui_event.outputs, out);
  Append(tree.nodes, on_ui_event);

  blender::bNode open = {};
  std::array<blender::bNodeSocket, 5> open_inputs = {};
  std::array<blender::bNodeSocket, 4> open_outputs = {};
  blender::bNodeSocketValueInt open_user_default = {};
  InitNode(open, "LogicNativeOpenObjectMenu", "Open Object Menu", 2);
  InitSocket(open_inputs[0], "Flow", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  InitSocket(open_inputs[1], "Camera", blender::SOCK_OBJECT, &camera_default);
  InitSocket(open_inputs[2], "Collection", blender::SOCK_COLLECTION, &collection_default);
  InitSocket(open_inputs[3], "Input Context", blender::SOCK_STRING, &context_default);
  InitSocket(open_inputs[4], "User", blender::SOCK_INT, &open_user_default);
  InitSocket(open_outputs[0], "Opened", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  InitSocket(open_outputs[1], "Failed", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  InitSocket(open_outputs[2], "Menu", blender::SOCK_INT, nullptr);
  InitSocket(open_outputs[3], "Result", blender::SOCK_INT, nullptr);
  for (blender::bNodeSocket &socket : open_inputs) {
    Append(open.inputs, socket);
  }
  for (blender::bNodeSocket &socket : open_outputs) {
    Append(open.outputs, socket);
  }
  Append(tree.nodes, open);

  blender::bNode print = {};
  blender::bNodeSocket print_flow = {};
  blender::bNodeSocket print_message = {};
  InitNode(print, "LogicNativePrint", "Print", 3);
  InitSocket(print_flow, "Flow", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  InitSocketWithIdentifier(print_message,
                           "Message",
                           "Value",
                           blender::SOCK_CUSTOM,
                           &message_default,
                           "NodeSocketLogicGeneric");
  Append(print.inputs, print_flow);
  Append(print.inputs, print_message);
  Append(tree.nodes, print);

  std::array<blender::bNodeLink, 2> links = {};
  InitLink(links[0], on_ui_event, out, open, open_inputs[0]);
  InitLink(links[1], open, open_outputs[0], print, print_flow);
  for (blender::bNodeLink &link : links) {
    Append(tree.links, link);
  }

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(program, nullptr);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  const LN_Instruction *open_instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnUpdate), LN_OpCode::OpenObjectMenu);
  const LN_Instruction *print_instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnUpdate), LN_OpCode::Print);
  ASSERT_NE(open_instruction, nullptr);
  ASSERT_NE(print_instruction, nullptr);
  /* A paused game can open a settings menu from a pause menu button and react to Opened. */
  EXPECT_NE(open_instruction->flags & LN_INSTRUCTION_FLAG_UI_EVENT_ROUTE, 0);
  EXPECT_NE(print_instruction->flags & LN_INSTRUCTION_FLAG_UI_EVENT_ROUTE, 0);
}

TEST(LN_TreeCompiler, AUIEventRouteContinuesThroughWaitForNextFrame)
{
  blender::bNodeTree tree = {};
  InitTree(tree);
  blender::bNodeSocketValueString action_default = {};
  blender::bNodeSocketValueString message_default = {};
  SetCString(message_default.value, "next frame");

  blender::bNode on_ui_event = {};
  blender::bNodeSocket action_input = {};
  blender::bNodeSocket out = {};
  InitNode(on_ui_event, "LogicNativeOnUIEvent", "On UI Event", 1);
  on_ui_event.custom1 = int16_t(LN_UIElementEvent::Click);
  InitSocket(action_input, "Action", blender::SOCK_STRING, &action_default);
  InitSocket(out, "Out", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  Append(on_ui_event.inputs, action_input);
  Append(on_ui_event.outputs, out);
  Append(tree.nodes, on_ui_event);

  blender::bNode next_frame = {};
  blender::bNodeSocket next_frame_flow = {};
  blender::bNodeSocket next_frame_completed = {};
  InitNode(next_frame, "LogicNativeOnNextFrame", "Wait for Next Frame", 2);
  InitSocket(next_frame_flow, "Flow", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  InitSocket(next_frame_completed,
             "Completed",
             blender::SOCK_BOOLEAN,
             nullptr,
             "NodeSocketLogicExecution");
  Append(next_frame.inputs, next_frame_flow);
  Append(next_frame.outputs, next_frame_completed);
  Append(tree.nodes, next_frame);

  blender::bNode print = {};
  blender::bNodeSocket print_flow = {};
  blender::bNodeSocket print_message = {};
  InitNode(print, "LogicNativePrint", "Print", 3);
  InitSocket(print_flow, "Flow", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  InitSocketWithIdentifier(print_message,
                           "Message",
                           "Value",
                           blender::SOCK_CUSTOM,
                           &message_default,
                           "NodeSocketLogicGeneric");
  Append(print.inputs, print_flow);
  Append(print.inputs, print_message);
  Append(tree.nodes, print);

  std::array<blender::bNodeLink, 2> links = {};
  InitLink(links[0], on_ui_event, out, next_frame, next_frame_flow);
  InitLink(links[1], next_frame, next_frame_completed, print, print_flow);
  for (blender::bNodeLink &link : links) {
    Append(tree.links, link);
  }

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(program, nullptr);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  ASSERT_TRUE(program->ValidateInstructionPayloads());
  const LN_Instruction *arm_instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnUpdate), LN_OpCode::ArmNextFrame);
  const LN_Instruction *print_instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnUpdate), LN_OpCode::Print);
  ASSERT_NE(arm_instruction, nullptr);
  ASSERT_NE(print_instruction, nullptr);
  /* A pause menu button can wait a frame before acting, also while the game is paused. */
  EXPECT_NE(arm_instruction->flags & LN_INSTRUCTION_FLAG_UI_EVENT_ROUTE, 0);
  EXPECT_NE(print_instruction->flags & LN_INSTRUCTION_FLAG_UI_EVENT_ROUTE, 0);
}

/** Runs Logic + Physics steps of a compiled tree and counts the Print commands of the last one,
 * all of them or those that print \a message. */
size_t CountPrintsOfOneFixedStep(const std::shared_ptr<LN_Program> &program,
                                 const int steps = 1,
                                 const char *message = nullptr)
{
  LN_RuntimeTree runtime_tree(program, reinterpret_cast<KX_GameObject *>(uintptr_t(64)), 0, 0);
  size_t prints = 0;
  for (int step = 1; step <= steps; step++) {
    LN_TickContext context;
    context.event = LN_Event::OnFixedUpdate;
    context.tick_index = uint64_t(step);
    LN_CommandBuffer command_buffer;
    command_buffer.BeginRecording();
    runtime_tree.ExecuteReady(command_buffer, context);
    command_buffer.EndRecording();
    prints = 0;
    for (const LN_CommandBuffer::Command &command : command_buffer.TakeRecordedCommands()) {
      prints += command.type == LN_CommandBuffer::CommandType::Print &&
                        (message == nullptr || command.property_name == message) ?
                    1 :
                    0;
    }
  }
  return prints;
}

/* A route inside a Loop body (a data node's Done, a Branch) runs in every iteration, also when
 * another route of the same event sorts next to the body. */
TEST(LN_TreeCompiler, ARouteInsideALoopBodyRunsInEveryIteration)
{
  for (const char *event_idname : {"LogicNativeOnFixedUpdate", "LogicNativeOnInit"}) {
    for (const char *middle_idname : {"LogicNativeListAppend", "LogicNativeBranch"}) {
      /* 0: no other route, 1: compiled before the Loop chain, 2: after it. */
      for (const int other_route : {0, 1, 2}) {
        SCOPED_TRACE(std::string(event_idname) + " " + middle_idname + " other route " +
                     std::to_string(other_route));
        blender::bNodeTree tree = {};
        InitTree(tree);

        blender::bNode event = {};
        blender::bNodeSocket event_out = {};
        InitNode(event, event_idname, "Event", 1);
        InitSocket(event_out, "Out", blender::SOCK_BOOLEAN, nullptr);
        Append(event.outputs, event_out);
        Append(tree.nodes, event);

        blender::bNodeSocketValueString other_message = {};
        SetCString(other_message.value, "other");
        blender::bNode other_print = {};
        blender::bNodeSocket other_print_flow = {};
        blender::bNodeSocket other_print_value = {};
        InitNode(other_print, "LogicNativePrint", "Other Print", 5);
        InitSocket(other_print_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
        InitSocket(other_print_value, "Value", blender::SOCK_STRING, &other_message);
        Append(other_print.inputs, other_print_flow);
        Append(other_print.inputs, other_print_value);
        blender::bNodeLink to_other = {};
        InitLink(to_other, event, event_out, other_print, other_print_flow);
        if (other_route == 1) {
          Append(tree.nodes, other_print);
          Append(tree.links, to_other);
        }

        blender::bNodeSocketValueInt count_default = {};
        count_default.value = 3;
        blender::bNodeSocketValueInt ticks_default = {};
        ticks_default.value = 1;
        blender::bNode loop = {};
        blender::bNodeSocket loop_flow = {};
        blender::bNodeSocket loop_count = {};
        blender::bNodeSocket loop_ticks = {};
        blender::bNodeSocket loop_out = {};
        InitNode(loop, "LogicNativeLoop", "Loop", 2);
        InitSocket(loop_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
        InitSocket(loop_count, "Count", blender::SOCK_INT, &count_default);
        InitSocket(loop_ticks, "Ticks to Complete", blender::SOCK_INT, &ticks_default);
        InitSocket(loop_out, "Loop", blender::SOCK_BOOLEAN, nullptr);
        Append(loop.inputs, loop_flow);
        Append(loop.inputs, loop_count);
        Append(loop.inputs, loop_ticks);
        Append(loop.outputs, loop_out);
        Append(tree.nodes, loop);

        /* List Append: Flow, List, Value -> Done, List. Branch: Flow, Condition -> True. */
        const bool branch = std::strcmp(middle_idname, "LogicNativeBranch") == 0;
        blender::bNodeSocketValueBoolean condition_default = {};
        condition_default.value = true;
        blender::bNode middle = {};
        blender::bNodeSocket middle_flow = {};
        blender::bNodeSocket middle_second = {};
        blender::bNodeSocket middle_value = {};
        blender::bNodeSocket middle_out = {};
        blender::bNodeSocket middle_list = {};
        InitNode(middle, middle_idname, "Middle", 3);
        InitSocket(middle_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
        Append(middle.inputs, middle_flow);
        if (branch) {
          InitSocket(middle_second, "Condition", blender::SOCK_BOOLEAN, &condition_default);
          InitSocket(middle_out, "True", blender::SOCK_BOOLEAN, nullptr);
          Append(middle.inputs, middle_second);
          Append(middle.outputs, middle_out);
        }
        else {
          InitSocket(middle_second, "List", blender::SOCK_CUSTOM, nullptr);
          InitSocketWithIdentifier(middle_value, "Value", "", blender::SOCK_CUSTOM, nullptr);
          InitSocket(middle_out, "Done", blender::SOCK_BOOLEAN, nullptr);
          InitSocket(middle_list, "List", blender::SOCK_CUSTOM, nullptr);
          Append(middle.inputs, middle_second);
          Append(middle.inputs, middle_value);
          Append(middle.outputs, middle_out);
          Append(middle.outputs, middle_list);
        }
        Append(tree.nodes, middle);

        blender::bNodeSocketValueString message_default = {};
        SetCString(message_default.value, "iteration");
        blender::bNode print = {};
        blender::bNodeSocket print_flow = {};
        blender::bNodeSocket print_value = {};
        InitNode(print, "LogicNativePrint", "Print", 4);
        InitSocket(print_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
        InitSocket(print_value, "Value", blender::SOCK_STRING, &message_default);
        Append(print.inputs, print_flow);
        Append(print.inputs, print_value);
        Append(tree.nodes, print);

        blender::bNodeLink to_loop = {};
        blender::bNodeLink to_middle = {};
        blender::bNodeLink to_print = {};
        InitLink(to_loop, event, event_out, loop, loop_flow);
        InitLink(to_middle, loop, loop_out, middle, middle_flow);
        InitLink(to_print, middle, middle_out, print, print_flow);
        Append(tree.links, to_loop);
        Append(tree.links, to_middle);
        Append(tree.links, to_print);
        if (other_route == 2) {
          Append(tree.nodes, other_print);
          Append(tree.links, to_other);
        }

        const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
        EXPECT_FALSE(program->GetCompileReport().HasErrors())
            << FormatCompileReport(program->GetCompileReport());
        ASSERT_TRUE(program->ValidateInstructionPayloads());
        EXPECT_EQ(CountPrintsOfOneFixedStep(program), other_route != 0 ? 4u : 3u);
      }
    }
  }
}

/* A Loop fed by two paths has a body for each: whichever path starts it, the body runs once per
 * iteration. */
TEST(LN_TreeCompiler, ALoopFedByTwoPathsRunsItsBodyOncePerIteration)
{
  for (const auto &[a_on, b_on] : {std::pair{true, false}, std::pair{false, true},
                                   std::pair{true, true}})
  {
    SCOPED_TRACE(std::string("A ") + (a_on ? "on" : "off") + ", B " + (b_on ? "on" : "off"));
    blender::bNodeTree tree = {};
    InitTree(tree);

    blender::bNode event = {};
    blender::bNodeSocket event_out = {};
    InitNode(event, "LogicNativeOnFixedUpdate", "Event", 1);
    InitSocket(event_out, "Out", blender::SOCK_BOOLEAN, nullptr);
    Append(event.outputs, event_out);
    Append(tree.nodes, event);

    blender::bNodeSocketValueBoolean a_default = {};
    a_default.value = a_on;
    blender::bNodeSocketValueBoolean b_default = {};
    b_default.value = b_on;
    blender::bNode branches[2] = {};
    blender::bNodeSocket branch_flow[2] = {};
    blender::bNodeSocket branch_condition[2] = {};
    blender::bNodeSocket branch_true[2] = {};
    for (int index = 0; index < 2; index++) {
      InitNode(branches[index], "LogicNativeBranch", index == 0 ? "Branch A" : "Branch B", 2 + index);
      InitSocket(branch_flow[index], "Flow", blender::SOCK_BOOLEAN, nullptr);
      InitSocket(branch_condition[index],
                 "Condition",
                 blender::SOCK_BOOLEAN,
                 index == 0 ? &a_default : &b_default);
      InitSocket(branch_true[index], "True", blender::SOCK_BOOLEAN, nullptr);
      Append(branches[index].inputs, branch_flow[index]);
      Append(branches[index].inputs, branch_condition[index]);
      Append(branches[index].outputs, branch_true[index]);
      Append(tree.nodes, branches[index]);
    }

    blender::bNode any = {};
    blender::bNodeSocket any_in = {};
    blender::bNodeSocket any_out = {};
    InitNode(any, "LogicNativeAnyFlow", "Any Flow", 4);
    InitSocket(any_in, "In", blender::SOCK_BOOLEAN, nullptr);
    InitSocket(any_out, "Out", blender::SOCK_BOOLEAN, nullptr);
    Append(any.inputs, any_in);
    Append(any.outputs, any_out);
    Append(tree.nodes, any);

    blender::bNodeSocketValueInt count_default = {};
    count_default.value = 3;
    blender::bNodeSocketValueInt ticks_default = {};
    ticks_default.value = 1;
    blender::bNode loop = {};
    blender::bNodeSocket loop_flow = {};
    blender::bNodeSocket loop_count = {};
    blender::bNodeSocket loop_ticks = {};
    blender::bNodeSocket loop_out = {};
    blender::bNodeSocket loop_completed = {};
    InitNode(loop, "LogicNativeLoop", "Loop", 5);
    InitSocket(loop_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
    InitSocket(loop_count, "Count", blender::SOCK_INT, &count_default);
    InitSocket(loop_ticks, "Ticks to Complete", blender::SOCK_INT, &ticks_default);
    InitSocket(loop_out, "Loop", blender::SOCK_BOOLEAN, nullptr);
    InitSocket(loop_completed, "Completed", blender::SOCK_BOOLEAN, nullptr);
    Append(loop.inputs, loop_flow);
    Append(loop.inputs, loop_count);
    Append(loop.inputs, loop_ticks);
    Append(loop.outputs, loop_out);
    Append(loop.outputs, loop_completed);
    Append(tree.nodes, loop);

    blender::bNodeSocketValueString done_message = {};
    SetCString(done_message.value, "done");
    blender::bNode done_print = {};
    blender::bNodeSocket done_flow = {};
    blender::bNodeSocket done_value = {};
    InitNode(done_print, "LogicNativePrint", "Done Print", 7);
    InitSocket(done_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
    InitSocket(done_value, "Value", blender::SOCK_STRING, &done_message);
    Append(done_print.inputs, done_flow);
    Append(done_print.inputs, done_value);
    Append(tree.nodes, done_print);

    blender::bNodeSocketValueString message_default = {};
    SetCString(message_default.value, "iteration");
    blender::bNode print = {};
    blender::bNodeSocket print_flow = {};
    blender::bNodeSocket print_value = {};
    InitNode(print, "LogicNativePrint", "Print", 6);
    InitSocket(print_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
    InitSocket(print_value, "Value", blender::SOCK_STRING, &message_default);
    Append(print.inputs, print_flow);
    Append(print.inputs, print_value);
    Append(tree.nodes, print);

    blender::bNodeLink links[7] = {};
    InitLink(links[0], event, event_out, branches[0], branch_flow[0]);
    InitLink(links[1], event, event_out, branches[1], branch_flow[1]);
    InitLink(links[2], branches[0], branch_true[0], any, any_in);
    InitLink(links[3], branches[1], branch_true[1], any, any_in);
    InitLink(links[4], any, any_out, loop, loop_flow);
    InitLink(links[5], loop, loop_out, print, print_flow);
    InitLink(links[6], loop, loop_completed, done_print, done_flow);
    for (blender::bNodeLink &link : links) {
      Append(tree.links, link);
    }

    const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
    EXPECT_FALSE(program->GetCompileReport().HasErrors())
        << FormatCompileReport(program->GetCompileReport());
    ASSERT_TRUE(program->ValidateInstructionPayloads());
    EXPECT_EQ(CountPrintsOfOneFixedStep(program, 1, "iteration"), 3u);
    EXPECT_EQ(CountPrintsOfOneFixedStep(program, 1, "done"), 1u);
  }
}

/* On Fixed Update and On Init into one Loop: every later step runs the body once per iteration,
 * with or without Completed wired, whichever input is linked first. */
TEST(LN_TreeCompiler, ALoopFedByOnInitAndFixedUpdateRunsItsBodyOnce)
{
  for (const bool fixed_first : {true, false}) {
    for (const bool with_completed : {false, true}) {
      SCOPED_TRACE(std::string(fixed_first ? "Fixed first" : "Init first") +
                   (with_completed ? ", with Completed" : ""));
      blender::bNodeTree tree = {};
      InitTree(tree);
      blender::bNode fixed = {};
      blender::bNode init = {};
      blender::bNodeSocket fixed_out = {};
      blender::bNodeSocket init_out = {};
      InitNode(fixed, "LogicNativeOnFixedUpdate", "On Fixed Update", 1);
      InitNode(init, "LogicNativeOnInit", "On Init", 2);
      InitSocket(fixed_out, "Out", blender::SOCK_BOOLEAN, nullptr);
      InitSocket(init_out, "Out", blender::SOCK_BOOLEAN, nullptr);
      Append(fixed.outputs, fixed_out);
      Append(init.outputs, init_out);
      Append(tree.nodes, fixed);
      Append(tree.nodes, init);

      blender::bNode any = {};
      blender::bNodeSocket any_in = {};
      blender::bNodeSocket any_out = {};
      InitNode(any, "LogicNativeAnyFlow", "Any Flow", 3);
      InitSocket(any_in, "In", blender::SOCK_BOOLEAN, nullptr);
      InitSocket(any_out, "Out", blender::SOCK_BOOLEAN, nullptr);
      Append(any.inputs, any_in);
      Append(any.outputs, any_out);
      Append(tree.nodes, any);

      blender::bNodeSocketValueInt count_default = {};
      count_default.value = 3;
      blender::bNodeSocketValueInt ticks_default = {};
      ticks_default.value = 1;
      blender::bNode loop = {};
      blender::bNodeSocket loop_flow = {};
      blender::bNodeSocket loop_count = {};
      blender::bNodeSocket loop_ticks = {};
      blender::bNodeSocket loop_out = {};
      blender::bNodeSocket loop_completed = {};
      InitNode(loop, "LogicNativeLoop", "Loop", 4);
      InitSocket(loop_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
      InitSocket(loop_count, "Count", blender::SOCK_INT, &count_default);
      InitSocket(loop_ticks, "Ticks to Complete", blender::SOCK_INT, &ticks_default);
      InitSocket(loop_out, "Loop", blender::SOCK_BOOLEAN, nullptr);
      InitSocket(loop_completed, "Completed", blender::SOCK_BOOLEAN, nullptr);
      Append(loop.inputs, loop_flow);
      Append(loop.inputs, loop_count);
      Append(loop.inputs, loop_ticks);
      Append(loop.outputs, loop_out);
      Append(loop.outputs, loop_completed);
      Append(tree.nodes, loop);

      blender::bNodeSocketValueString message_default = {};
      SetCString(message_default.value, "iteration");
      blender::bNodeSocketValueString done_message = {};
      SetCString(done_message.value, "done");
      blender::bNode prints[2] = {};
      blender::bNodeSocket print_flow[2] = {};
      blender::bNodeSocket print_value[2] = {};
      for (int index = 0; index < 2; index++) {
        InitNode(prints[index], "LogicNativePrint", index == 0 ? "Print" : "Done Print", 5 + index);
        InitSocket(print_flow[index], "Flow", blender::SOCK_BOOLEAN, nullptr);
        InitSocket(print_value[index],
                   "Value",
                   blender::SOCK_STRING,
                   index == 0 ? &message_default : &done_message);
        Append(prints[index].inputs, print_flow[index]);
        Append(prints[index].inputs, print_value[index]);
        Append(tree.nodes, prints[index]);
      }

      blender::bNodeLink links[5] = {};
      InitLink(links[0], fixed_first ? fixed : init, fixed_first ? fixed_out : init_out, any, any_in);
      InitLink(links[1], fixed_first ? init : fixed, fixed_first ? init_out : fixed_out, any, any_in);
      InitLink(links[2], any, any_out, loop, loop_flow);
      InitLink(links[3], loop, loop_out, prints[0], print_flow[0]);
      InitLink(links[4], loop, loop_completed, prints[1], print_flow[1]);
      for (int index = 0; index < (with_completed ? 5 : 4); index++) {
        Append(tree.links, links[index]);
      }

      const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
      EXPECT_FALSE(program->GetCompileReport().HasErrors())
          << FormatCompileReport(program->GetCompileReport());
      ASSERT_TRUE(program->ValidateInstructionPayloads());
      EXPECT_EQ(CountPrintsOfOneFixedStep(program, 3, "iteration"), 3u);
      EXPECT_EQ(CountPrintsOfOneFixedStep(program, 3, "done"), with_completed ? 1u : 0u);
    }
  }
}

/* A node for compile tests that keeps its sockets and their defaults alive with it. Declare it
 * as a local: the tree points at it. */
struct TestNode {
  blender::bNode node = {};
  std::deque<blender::bNodeSocket> sockets;
  std::deque<blender::bNodeSocketValueFloat> floats;
  std::deque<blender::bNodeSocketValueInt> ints;
  std::deque<blender::bNodeSocketValueBoolean> bools;
  std::deque<blender::bNodeSocketValueString> strings;
  std::deque<blender::bNodeSocketValueObject> objects;
  std::deque<blender::bNodeSocketValueVector> vectors;

  TestNode(blender::bNodeTree &tree, const char *idname, const char *name, const int32_t id)
  {
    InitNode(node, idname, name, id);
    Append(tree.nodes, node);
  }
  TestNode(const TestNode &) = delete;

  blender::bNodeSocket &add(const bool input,
                            const char *name,
                            const blender::eNodeSocketDatatype type,
                            void *default_value,
                            const char *idname = nullptr)
  {
    blender::bNodeSocket &socket = sockets.emplace_back();
    InitSocket(socket, name, type, default_value, idname);
    Append(input ? node.inputs : node.outputs, socket);
    return socket;
  }
  blender::bNodeSocket &flow(const bool input, const char *name)
  {
    return add(input, name, blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicExecution");
  }
  blender::bNodeSocket &in_float(const char *name, const float value)
  {
    floats.emplace_back().value = value;
    return add(true, name, blender::SOCK_FLOAT, &floats.back());
  }
  blender::bNodeSocket &in_int(const char *name, const int value)
  {
    ints.emplace_back().value = value;
    return add(true, name, blender::SOCK_INT, &ints.back());
  }
  blender::bNodeSocket &in_bool(const char *name, const bool value)
  {
    bools.emplace_back().value = value;
    return add(true, name, blender::SOCK_BOOLEAN, &bools.back());
  }
  blender::bNodeSocket &in_string(const char *name, const char *value)
  {
    SetCString(strings.emplace_back().value, value);
    return add(true, name, blender::SOCK_STRING, &strings.back());
  }
  blender::bNodeSocket &in_object(const char *name)
  {
    return add(true, name, blender::SOCK_OBJECT, &objects.emplace_back());
  }
  blender::bNodeSocket &in_vector(const char *name, const MT_Vector3 &value)
  {
    blender::bNodeSocketValueVector &vector = vectors.emplace_back();
    vector.dimensions = 3;
    vector.value[0] = value.x();
    vector.value[1] = value.y();
    vector.value[2] = value.z();
    return add(true, name, blender::SOCK_VECTOR, &vector);
  }
};

/* A game property read while the game runs: the Value output. */
blender::bNodeSocket &AddPropertyRead(TestNode &read, const char *property)
{
  read.in_object("Object");
  read.in_string("Property", property);
  return read.add(false, "Value", blender::SOCK_CUSTOM, nullptr, "NodeSocketLogicGeneric");
}

/* Outputs that once folded only constants also read values that change while the game runs. */
TEST(LN_TreeCompiler, WithinRangeAndKeyLoggerReadRuntimeValues)
{
  blender::bNodeTree tree = {};
  InitTree(tree);
  TestNode on_update(tree, "LogicNativeOnUpdate", "On Update", 1);
  blender::bNodeSocket &tick = on_update.flow(false, "Out");
  TestNode speed(tree, "LogicNativeGetGameProperty", "Get Object Property", 2);
  blender::bNodeSocket &speed_value = AddPropertyRead(speed, "speed");
  TestNode range(tree, "LogicNativeWithinRange", "Within Range", 3);
  blender::bNodeSocket &range_value = range.in_float("Value", 0.0f);
  range.in_float("Min", 1.0f);
  range.in_float("Max", 5.0f);
  blender::bNodeSocket &inside = range.add(false, "Result", blender::SOCK_BOOLEAN, nullptr);
  TestNode branch(tree, "LogicNativeBranch", "Branch", 4);
  blender::bNodeSocket &branch_flow = branch.flow(true, "Flow");
  blender::bNodeSocket &condition = branch.in_bool("Condition", false);
  blender::bNodeSocket &branch_true = branch.flow(false, "True");
  branch.flow(false, "False");
  TestNode logger(tree, "LogicNativeKeyLogger", "Key Logger", 5);
  logger.in_bool("Only Characters", true);
  logger.add(false, "Pressed", blender::SOCK_BOOLEAN, nullptr);
  blender::bNodeSocket &character = logger.add(false, "Character", blender::SOCK_STRING, nullptr);
  logger.add(false, "Keycode", blender::SOCK_STRING, nullptr);
  TestNode print(tree, "LogicNativePrint", "Print", 6);
  blender::bNodeSocket &print_flow = print.add(true, "Flow", blender::SOCK_BOOLEAN, nullptr);
  blender::bNodeSocket &print_value = print.add(true, "Value", blender::SOCK_CUSTOM, nullptr);
  std::array<blender::bNodeLink, 5> links = {};
  InitLink(links[0], on_update.node, tick, branch.node, branch_flow);
  InitLink(links[1], speed.node, speed_value, range.node, range_value);
  InitLink(links[2], range.node, inside, branch.node, condition);
  InitLink(links[3], branch.node, branch_true, print.node, print_flow);
  InitLink(links[4], logger.node, character, print.node, print_value);
  for (blender::bNodeLink &link : links) {
    Append(tree.links, link);
  }

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(program, nullptr);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  std::vector<std::string> validation_errors;
  EXPECT_TRUE(program->ValidateInstructionPayloads(&validation_errors))
      << (validation_errors.empty() ? std::string() : validation_errors.front());
  /* Inside: Min < Value and Value < Max, compared while the game runs. */
  const std::vector<LN_Instruction> &instructions = program->GetInstructions(LN_Event::OnUpdate);
  const LN_Instruction *route = FindInstruction(instructions, LN_OpCode::BranchRoute);
  ASSERT_NE(route, nullptr);
  const LN_BoolExpression *both = BoolExpressionAt(*program, route->bool_expr_index);
  ASSERT_NE(both, nullptr);
  EXPECT_EQ(both->kind, LN_BoolExpressionKind::And);
  for (const uint32_t side : {both->input0, both->input1}) {
    const LN_BoolExpression *compare = BoolExpressionAt(*program, side);
    ASSERT_NE(compare, nullptr);
    EXPECT_EQ(compare->kind, LN_BoolExpressionKind::FloatCompare);
  }
  /* The typed character reaches the text it feeds. */
  const LN_Instruction *print_instruction = FindInstruction(instructions, LN_OpCode::Print);
  ASSERT_NE(print_instruction, nullptr);
  const LN_ValueExpression *shown = ValueExpressionAt(*program, print_instruction->value_expr_index);
  ASSERT_NE(shown, nullptr);
  ASSERT_EQ(shown->kind, LN_ValueExpressionKind::FromString);
  const LN_StringExpression *typed = StringExpressionAt(*program, shown->input0);
  ASSERT_NE(typed, nullptr);
  EXPECT_EQ(typed->kind, LN_StringExpressionKind::KeyLoggerCharacter);
}

TEST(LN_TreeCompiler, AnimationStatusAndALinkedStickInvertReachTheirReaders)
{
  blender::bNodeTree tree = {};
  InitTree(tree);
  TestNode on_update(tree, "LogicNativeOnUpdate", "On Update", 1);
  blender::bNodeSocket &tick = on_update.flow(false, "Out");
  TestNode status(tree, "LogicNativeAnimationStatus", "Animation Status", 2);
  status.in_object("Object");
  status.in_int("Layer", 0);
  status.add(false, "Is Playing", blender::SOCK_BOOLEAN, nullptr);
  blender::bNodeSocket &action_name = status.add(false, "Action Name", blender::SOCK_STRING, nullptr);
  blender::bNodeSocket &action_frame = status.add(false, "Action Frame", blender::SOCK_FLOAT, nullptr);
  TestNode invert(tree, "LogicNativeGetGameProperty", "Get Object Property", 3);
  blender::bNodeSocket &invert_value = AddPropertyRead(invert, "invert_y");
  TestNode stick(tree, "LogicNativeGamepadStick", "Gamepad Sticks", 4);
  stick.in_bool("Invert X", false);
  blender::bNodeSocket &invert_y = stick.in_bool("Invert Y", true);
  stick.in_int("Index", 0);
  stick.in_float("Sensitivity", 1.0f);
  stick.in_float("Threshold", 0.1f);
  stick.add(false, "X", blender::SOCK_FLOAT, nullptr);
  stick.add(false, "Y", blender::SOCK_FLOAT, nullptr);
  blender::bNodeSocket &stick_vector = stick.add(false, "Vector", blender::SOCK_VECTOR, nullptr);
  SetCString(stick_vector.identifier, "VEC");
  std::array<TestNode *, 3> prints = {};
  TestNode print_name(tree, "LogicNativePrint", "Print", 5);
  TestNode print_frame(tree, "LogicNativePrint", "Print.001", 6);
  TestNode print_stick(tree, "LogicNativePrint", "Print.002", 7);
  prints = {&print_name, &print_frame, &print_stick};
  std::array<blender::bNodeSocket *, 3> sources = {&action_name, &action_frame, &stick_vector};
  std::array<blender::bNodeLink, 7> links = {};
  for (size_t index = 0; index < prints.size(); ++index) {
    blender::bNodeSocket &flow = prints[index]->add(true, "Flow", blender::SOCK_BOOLEAN, nullptr);
    blender::bNodeSocket &value = prints[index]->add(true, "Value", blender::SOCK_CUSTOM, nullptr);
    InitLink(links[index * 2], on_update.node, tick, prints[index]->node, flow);
    InitLink(links[index * 2 + 1],
             index < 2 ? status.node : stick.node,
             *sources[index],
             prints[index]->node,
             value);
  }
  InitLink(links[6], invert.node, invert_value, stick.node, invert_y);
  for (blender::bNodeLink &link : links) {
    Append(tree.links, link);
  }

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(program, nullptr);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  std::vector<std::string> validation_errors;
  EXPECT_TRUE(program->ValidateInstructionPayloads(&validation_errors))
      << (validation_errors.empty() ? std::string() : validation_errors.front());
  std::vector<const LN_ValueExpression *> shown;
  for (const LN_Instruction &instruction : program->GetInstructions(LN_Event::OnUpdate)) {
    if (instruction.opcode == LN_OpCode::Print) {
      shown.push_back(ValueExpressionAt(*program, instruction.value_expr_index));
    }
  }
  ASSERT_EQ(shown.size(), 3u);
  bool name_found = false;
  bool frame_found = false;
  bool flipped_found = false;
  for (const LN_ValueExpression *value : shown) {
    ASSERT_NE(value, nullptr);
    if (value->kind == LN_ValueExpressionKind::FromString) {
      const LN_StringExpression *name = StringExpressionAt(*program, value->input0);
      name_found = name != nullptr && name->kind == LN_StringExpressionKind::AnimationActionName;
    }
    else if (value->kind == LN_ValueExpressionKind::FromFloat) {
      const LN_FloatExpression *frame = FloatExpressionAt(*program, value->input0);
      frame_found = frame != nullptr && frame->kind == LN_FloatExpressionKind::AnimationFrame;
    }
    else if (value->kind == LN_ValueExpressionKind::FromVector) {
      /* The linked Invert Y flips the stick's vector while the game runs. */
      const LN_VectorExpression *flipped = VectorExpressionAt(*program, value->input0);
      flipped_found = flipped != nullptr && flipped->kind == LN_VectorExpressionKind::Multiply &&
                      VectorExpressionAt(*program, flipped->input0) != nullptr &&
                      VectorExpressionAt(*program, flipped->input0)->kind ==
                          LN_VectorExpressionKind::GamepadStick;
    }
  }
  EXPECT_TRUE(name_found);
  EXPECT_TRUE(frame_found);
  EXPECT_TRUE(flipped_found);
}

TEST(LN_TreeCompiler, KeyAndTreePropertyNamesFromRuntimeLinksAreErrors)
{
  blender::bNodeTree tree = {};
  InitTree(tree);
  TestNode on_update(tree, "LogicNativeOnUpdate", "On Update", 1);
  blender::bNodeSocket &tick = on_update.flow(false, "Out");
  TestNode key_name(tree, "LogicNativeGetGameProperty", "Get Object Property", 2);
  blender::bNodeSocket &key_value = AddPropertyRead(key_name, "jump_key");
  TestNode key(tree, "LogicNativeKeyboardKey", "Keyboard Key", 3);
  blender::bNodeSocket &key_input = key.in_string("Key", "A");
  blender::bNodeSocket &key_out = key.flow(false, "Out");
  key.add(false, "Active", blender::SOCK_BOOLEAN, nullptr);
  key.add(false, "Pressed", blender::SOCK_BOOLEAN, nullptr);
  key.add(false, "Released", blender::SOCK_BOOLEAN, nullptr);
  TestNode print_key(tree, "LogicNativePrint", "Print", 4);
  blender::bNodeSocket &print_key_flow = print_key.add(true, "Flow", blender::SOCK_BOOLEAN, nullptr);
  print_key.add(true, "Value", blender::SOCK_CUSTOM, nullptr);
  TestNode tree_name(tree, "LogicNativeGetGameProperty", "Get Object Property.001", 5);
  blender::bNodeSocket &tree_name_value = AddPropertyRead(tree_name, "which");
  TestNode tree_read(tree, "LogicNativeGetTreeProperty", "Get Tree Property", 6);
  blender::bNodeSocket &tree_read_name = tree_read.in_string("Property", "property");
  blender::bNodeSocket &tree_read_value = tree_read.add(
      false, "Value", blender::SOCK_CUSTOM, nullptr, "NodeSocketLogicGeneric");
  TestNode print_tree(tree, "LogicNativePrint", "Print.001", 7);
  blender::bNodeSocket &print_tree_flow = print_tree.add(true, "Flow", blender::SOCK_BOOLEAN, nullptr);
  blender::bNodeSocket &print_tree_value = print_tree.add(true, "Value", blender::SOCK_CUSTOM, nullptr);
  std::array<blender::bNodeLink, 5> links = {};
  InitLink(links[0], key_name.node, key_value, key.node, key_input);
  InitLink(links[1], key.node, key_out, print_key.node, print_key_flow);
  InitLink(links[2], tree_name.node, tree_name_value, tree_read.node, tree_read_name);
  InitLink(links[3], on_update.node, tick, print_tree.node, print_tree_flow);
  InitLink(links[4], tree_read.node, tree_read_value, print_tree.node, print_tree_value);
  for (blender::bNodeLink &link : links) {
    Append(tree.links, link);
  }

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(program, nullptr);
  const LN_CompileReport &report = program->GetCompileReport();
  /* Said out loud instead of a key that never fires and a property that reads nothing. */
  EXPECT_TRUE(HasErrorContaining(report, "Input Action takes keys")) << FormatCompileReport(report);
  EXPECT_TRUE(HasErrorContaining(report, "Get Tree Property requires a constant property name"))
      << FormatCompileReport(report);
}


/* A scene-graph node for a test object, so the UI can place and hit it. */
class RootRelation : public SG_ParentRelation {
 public:
  bool UpdateChildCoordinates(SG_Node *child, const SG_Node *, bool &parent_updated) override
  {
    if (child == nullptr) {
      return false;
    }
    child->SetWorldFromLocalTransform();
    parent_updated = true;
    return true;
  }
  SG_ParentRelation *NewCopy() override
  {
    return new RootRelation();
  }
};

template<typename ObjectType> struct UIObject {
  explicit UIObject(KX_Scene *scene) : node(&object, scene, callbacks)
  {
    node.SetParentRelation(new RootRelation());
    object.SetSGNode(&node);
  }
  ~UIObject()
  {
    object.SetSGNode(nullptr);
  }
  SG_Callbacks callbacks;
  ObjectType object;
  SG_Node node;
};

/* On UI Event runs its route once for every matching event of a frame, each time with that
 * event's outputs, also through a Loop in the route; a deferred result is not repeated. */
TEST(LN_TreeCompiler, OnUIEventRunsItsRouteOnceForEachEventOfAFrame)
{
  blender::bNodeTree tree = {};
  InitTree(tree);
  TestNode on_event(tree, "LogicNativeOnUIEvent", "On UI Event", 1);
  on_event.node.custom1 = int16_t(LN_UIElementEvent::Enter);
  on_event.in_string("Action", "");
  blender::bNodeSocket &out = on_event.flow(false, "Out");
  blender::bNodeSocket &action = on_event.add(false, "Action", blender::SOCK_STRING, nullptr);
  TestNode print_action(tree, "LogicNativePrint", "Print", 2);
  blender::bNodeSocket &print_action_flow = print_action.add(
      true, "Flow", blender::SOCK_BOOLEAN, nullptr);
  blender::bNodeSocket &print_action_value = print_action.add(
      true, "Value", blender::SOCK_CUSTOM, nullptr);
  TestNode loop(tree, "LogicNativeLoop", "Loop", 3);
  blender::bNodeSocket &loop_flow = loop.flow(true, "Flow");
  loop.in_int("Count", 2);
  loop.in_int("Ticks to Complete", 1);
  blender::bNodeSocket &loop_out = loop.flow(false, "Loop");
  blender::bNodeSocket &loop_done = loop.flow(false, "Completed");
  TestNode print_loop(tree, "LogicNativePrint", "Print.001", 4);
  blender::bNodeSocket &print_loop_flow = print_loop.add(true, "Flow", blender::SOCK_BOOLEAN, nullptr);
  print_loop.in_string("Value", "iteration");
  TestNode print_done(tree, "LogicNativePrint", "Print.002", 5);
  blender::bNodeSocket &print_done_flow = print_done.add(true, "Flow", blender::SOCK_BOOLEAN, nullptr);
  print_done.in_string("Value", "completed");
  std::array<blender::bNodeLink, 5> links = {};
  InitLink(links[0], on_event.node, out, print_action.node, print_action_flow);
  InitLink(links[1], on_event.node, action, print_action.node, print_action_value);
  InitLink(links[2], on_event.node, out, loop.node, loop_flow);
  InitLink(links[3], loop.node, loop_out, print_loop.node, print_loop_flow);
  InitLink(links[4], loop.node, loop_done, print_done.node, print_done_flow);
  for (blender::bNodeLink &link : links) {
    Append(tree.links, link);
  }
  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(program, nullptr);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  std::vector<std::string> validation_errors;
  EXPECT_TRUE(program->ValidateInstructionPayloads(&validation_errors))
      << (validation_errors.empty() ? std::string() : validation_errors.front());
  /* Every instruction of the route says which On UI Event started it. */
  const std::vector<LN_Instruction> &instructions = program->GetInstructions(LN_Event::OnUpdate);
  ASSERT_FALSE(instructions.empty());
  for (const LN_Instruction &instruction : instructions) {
    EXPECT_NE(instruction.ui_event_state_index, LN_INVALID_INDEX);
  }
  EXPECT_EQ(program->GetExecBlockProgram(LN_Event::OnUpdate).ui_event_states.size(), 1u);

  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  UIObject<KX_Camera> camera(scene);
  UIObject<KX_GameObject> first(scene);
  UIObject<KX_GameObject> second(scene);
  LN_UIService::ElementSettings first_settings;
  first_settings.action = "first";
  first_settings.pointer = LN_UIPropagation::PassThrough;
  LN_UIService::ElementSettings second_settings = first_settings;
  second_settings.action = "second";
  /* The topmost: its event comes first in the frame's list but runs last, so what it sets stays.
   */
  first_settings.priority = 10;
  std::vector<std::string> action_order;
  auto registration = [&](KX_GameObject &object, const bool hit) {
    LN_UIService::Registration result;
    result.object = &object;
    result.camera = &camera.object;
    result.ray_from = MT_Vector3(hit ? 0.0f : 5.0f, 0.0f, -2.0f);
    result.ray_to = MT_Vector3(hit ? 0.0f : 5.0f, 0.0f, 2.0f);
    return result;
  };
  DEV_InputDevice device;
  LN_InputSnapshot input;
  device.ConvertMoveEvent(100, 100);
  input.Capture(&device, nullptr);

  for (const bool registers : {false, true}) {
    SCOPED_TRACE(registers ? "register evaluator" : "tree evaluator");
    LN_RuntimeTree runtime_tree(program, reinterpret_cast<KX_GameObject *>(uintptr_t(0x70)), 0, 0);
    runtime_tree.SetLogicManager(&manager);
    uint64_t tick = registers ? 100 : 0;
    /* Which of the two elements the pointer is over this frame. */
    auto frame = [&](const bool first_hit, const bool second_hit) {
      ++tick;
      service.BeginTick(LN_Event::OnUpdate, tick, &input);
      service.RegisterElement(&first.object, first_settings, registration(first.object, first_hit));
      service.RegisterElement(
          &second.object, second_settings, registration(second.object, second_hit));
      service.ResolveInteractions();
      LN_TickContext context;
      context.event = LN_Event::OnUpdate;
      context.tick_index = tick;
      context.use_register_expression_evaluator = registers;
      LN_CommandBuffer command_buffer;
      command_buffer.BeginRecording();
      runtime_tree.ExecuteReady(command_buffer, context);
      command_buffer.EndRecording();
      std::vector<std::string> printed;
      action_order.clear();
      for (const LN_CommandBuffer::Command &command : command_buffer.TakeRecordedCommands()) {
        if (command.type == LN_CommandBuffer::CommandType::Print) {
          printed.push_back(command.property_name);
          if (command.property_name == "first" || command.property_name == "second") {
            action_order.push_back(command.property_name);
          }
        }
      }
      std::sort(printed.begin(), printed.end());
      return printed;
    };
    frame(false, false);
    /* Both entered in one frame: the route runs twice, each time with its own Action, and the
     * Loop runs for each. */
    EXPECT_EQ(frame(true, true),
              (std::vector<std::string>{"completed",
                                        "completed",
                                        "first",
                                        "iteration",
                                        "iteration",
                                        "iteration",
                                        "iteration",
                                        "second"}));
    EXPECT_EQ(action_order, (std::vector<std::string>{"second", "first"}));
    /* No event: nothing. */
    EXPECT_TRUE(frame(true, true).empty());
    frame(false, false);
    /* One event: once. */
    EXPECT_EQ(frame(false, true),
              (std::vector<std::string>{"completed", "iteration", "iteration", "second"}));
    frame(false, false);
    /* The next frame starts with its own first event again. */
    EXPECT_EQ(frame(true, false),
              (std::vector<std::string>{"completed", "first", "iteration", "iteration"}));
  }
}

/* On UI Sound: each output runs for its own sound, once for each player's sound of a frame, with
 * the outputs of that sound (Action, Menu, User). */
TEST(LN_TreeCompiler, OnUISoundRunsEachOutputForItsSoundOnceForEachPlayer)
{
  blender::bNodeTree tree = {};
  InitTree(tree);
  TestNode on_sound(tree, "LogicNativeOnUISound", "On UI Sound", 1);
  blender::bNodeSocket &move = on_sound.flow(false, "Move");
  blender::bNodeSocket &accept = on_sound.flow(false, "Accept");
  blender::bNodeSocket &back = on_sound.flow(false, "Back");
  on_sound.flow(false, "Denied");
  blender::bNodeSocket &action = on_sound.add(false, "Action", blender::SOCK_STRING, nullptr);
  blender::bNodeSocket &menu = on_sound.add(false, "Menu", blender::SOCK_STRING, nullptr);
  blender::bNodeSocket &user = on_sound.add(false, "User", blender::SOCK_INT, nullptr);
  /* Move prints the player, Accept the element's Action, Back the menu's name. */
  TestNode print_move(tree, "LogicNativePrint", "Print", 2);
  blender::bNodeSocket &print_move_flow = print_move.add(
      true, "Flow", blender::SOCK_BOOLEAN, nullptr);
  blender::bNodeSocket &print_move_value = print_move.add(
      true, "Value", blender::SOCK_CUSTOM, nullptr);
  TestNode print_accept(tree, "LogicNativePrint", "Print.001", 3);
  blender::bNodeSocket &print_accept_flow = print_accept.add(
      true, "Flow", blender::SOCK_BOOLEAN, nullptr);
  blender::bNodeSocket &print_accept_value = print_accept.add(
      true, "Value", blender::SOCK_CUSTOM, nullptr);
  TestNode print_back(tree, "LogicNativePrint", "Print.002", 4);
  blender::bNodeSocket &print_back_flow = print_back.add(
      true, "Flow", blender::SOCK_BOOLEAN, nullptr);
  blender::bNodeSocket &print_back_value = print_back.add(
      true, "Value", blender::SOCK_CUSTOM, nullptr);
  std::array<blender::bNodeLink, 6> links = {};
  InitLink(links[0], on_sound.node, move, print_move.node, print_move_flow);
  InitLink(links[1], on_sound.node, user, print_move.node, print_move_value);
  InitLink(links[2], on_sound.node, accept, print_accept.node, print_accept_flow);
  InitLink(links[3], on_sound.node, action, print_accept.node, print_accept_value);
  InitLink(links[4], on_sound.node, back, print_back.node, print_back_flow);
  InitLink(links[5], on_sound.node, menu, print_back.node, print_back_value);
  for (blender::bNodeLink &link : links) {
    Append(tree.links, link);
  }
  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(program, nullptr);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  std::vector<std::string> validation_errors;
  EXPECT_TRUE(program->ValidateInstructionPayloads(&validation_errors))
      << (validation_errors.empty() ? std::string() : validation_errors.front());
  /* Its routes are UI routes (they also run while paused), repeated for each sound. */
  const std::vector<LN_Instruction> &instructions = program->GetInstructions(LN_Event::OnUpdate);
  ASSERT_FALSE(instructions.empty());
  for (const LN_Instruction &instruction : instructions) {
    EXPECT_NE(instruction.ui_event_state_index, LN_INVALID_INDEX);
  }
  EXPECT_EQ(program->GetExecBlockProgram(LN_Event::OnUpdate).ui_event_states.size(), 1u);

  KX_Scene *scene = reinterpret_cast<KX_Scene *>(uintptr_t(1));
  LN_Manager manager(*scene);
  LN_UIService &service = manager.GetUIService();
  DEV_InputDevice device;
  LN_InputSnapshot input;
  input.Capture(&device, nullptr);
  auto sound =[](const LN_UISound kind, const int32_t player, const char *action, const char *in) {
    LN_UIService::SoundEvent event;
    event.sound = kind;
    event.user = player;
    event.action = action;
    event.menu = in;
    return event;
  };
  for (const bool registers : {false, true}) {
    SCOPED_TRACE(registers ? "register evaluator" : "tree evaluator");
    LN_RuntimeTree runtime_tree(program, reinterpret_cast<KX_GameObject *>(uintptr_t(0x70)), 0, 0);
    runtime_tree.SetLogicManager(&manager);
    uint64_t tick = registers ? 100 : 0;
    auto frame = [&](const std::vector<LN_UIService::SoundEvent> &sounds) {
      ++tick;
      service.BeginTick(LN_Event::OnUpdate, tick, &input);
      for (const LN_UIService::SoundEvent &event : sounds) {
        service.AddSoundEventForTests(event);
      }
      LN_TickContext context;
      context.event = LN_Event::OnUpdate;
      context.tick_index = tick;
      context.use_register_expression_evaluator = registers;
      LN_CommandBuffer command_buffer;
      command_buffer.BeginRecording();
      runtime_tree.ExecuteReady(command_buffer, context);
      command_buffer.EndRecording();
      std::vector<std::string> printed;
      for (const LN_CommandBuffer::Command &command : command_buffer.TakeRecordedCommands()) {
        if (command.type == LN_CommandBuffer::CommandType::Print) {
          printed.push_back(command.property_name);
        }
      }
      std::sort(printed.begin(), printed.end());
      return printed;
    };
    frame({});
    /* Player 0 clicks "play" while player 1's highlight moves: each output runs once. */
    EXPECT_EQ(frame({sound(LN_UISound::Accept, 0, "play", ""),
                     sound(LN_UISound::Move, 1, "slot", "Inventory")}),
              (std::vector<std::string>{"1", "play"}));
    /* Two players' Back in one frame: Back runs for each, with the menu each one closed. */
    EXPECT_EQ(frame({sound(LN_UISound::Back, 0, "", "Pause Menu"),
                     sound(LN_UISound::Back, 1, "", "Inventory")}),
              (std::vector<std::string>{"Inventory", "Pause Menu"}));
    /* Denied has no route here; no sound, no route. */
    EXPECT_TRUE(frame({sound(LN_UISound::Denied, 0, "slot", "")}).empty());
    EXPECT_TRUE(frame({}).empty());
  }
}

/* Set Volume: the group (Master by default) and the Volume and Fade inputs reach its command. */
TEST(LN_TreeCompiler, SetVolumeCompilesItsGroupVolumeAndFade)
{
  blender::bNodeTree tree = {};
  InitTree(tree);
  TestNode on_update(tree, "LogicNativeOnUpdate", "On Update", 1);
  blender::bNodeSocket &out = on_update.flow(false, "Out");
  TestNode set_volume(tree, "LogicNativeSetVolume", "Set Volume", 2);
  set_volume.node.custom1 = int16_t(LN_SoundGroup::Music);
  blender::bNodeSocket &flow = set_volume.flow(true, "Flow");
  set_volume.in_float("Volume", 0.3f);
  set_volume.in_float("Fade", 1.5f);
  set_volume.flow(false, "Done");
  blender::bNodeLink link = {};
  InitLink(link, on_update.node, out, set_volume.node, flow);
  Append(tree.links, link);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(program, nullptr);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  const LN_Instruction *instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnUpdate), LN_OpCode::SetSoundVolume);
  ASSERT_NE(instruction, nullptr);
  EXPECT_EQ(instruction->int_value, int32_t(LN_SoundGroup::Music));

  LN_RuntimeTree runtime_tree(program, reinterpret_cast<KX_GameObject *>(uintptr_t(0x70)), 0, 0);
  LN_TickContext context;
  context.event = LN_Event::OnUpdate;
  context.tick_index = 1;
  LN_CommandBuffer command_buffer;
  command_buffer.BeginRecording();
  runtime_tree.ExecuteReady(command_buffer, context);
  command_buffer.EndRecording();
  const std::vector<LN_CommandBuffer::Command> commands = command_buffer.TakeRecordedCommands();
  ASSERT_EQ(commands.size(), 1u);
  EXPECT_EQ(commands[0].type, LN_CommandBuffer::CommandType::SetSoundVolume);
  EXPECT_EQ(commands[0].sound_group, uint8_t(LN_SoundGroup::Music));
  EXPECT_FLOAT_EQ(commands[0].vector_value.x(), 0.3f);
  EXPECT_FLOAT_EQ(commands[0].vector_value.y(), 1.5f);
}

/* The new UI outputs and node reach the runtime: Accepted, Page and Page Count, Scroll Object
 * View with its mode and Done. */
TEST(LN_TreeCompiler, AcceptedPagesAndScrollObjectViewCompile)
{
  blender::bNodeTree tree = {};
  InitTree(tree);
  TestNode on_event(tree, "LogicNativeOnUIEvent", "On UI Event", 1);
  on_event.node.custom1 = int16_t(LN_UIElementEvent::DropEnter);
  on_event.in_string("Action", "slot");
  blender::bNodeSocket &out = on_event.flow(false, "Out");
  blender::bNodeSocket &accepted = on_event.add(false, "Accepted", blender::SOCK_BOOLEAN, nullptr);
  TestNode grid(tree, "LogicNativeObjectGridView", "Object Grid View", 2);
  blender::bNodeSocketValueVector no_spacing = {};
  grid.in_object("Viewport Object");
  grid.in_object("Camera");
  grid.add(true, "Slots", blender::SOCK_CUSTOM, nullptr, "NodeSocketLogicGeneric");
  grid.add(true, "Items", blender::SOCK_CUSTOM, nullptr, "NodeSocketLogicGeneric");
  grid.in_int("First Item", 0);
  grid.in_int("Columns", 1);
  grid.in_int("Wheel Rows", 1);
  grid.in_bool("Enabled", true);
  grid.in_bool("Cull Outside", true);
  grid.in_bool("Focus Follow", true);
  grid.in_int("User", 0);
  grid.in_int("Priority", 0);
  grid.in_float("Padding", 0.0f);
  grid.in_bool("Show Counts", true);
  grid.in_bool("Show Names", false);
  grid.in_bool("Show Empty", false);
  grid.in_object("Slot Template");
  grid.in_int("Rows", 0);
  grid.add(true, "Spacing", blender::SOCK_VECTOR, &no_spacing);
  grid.in_object("Scrollbar");
  GridListSockets grid_list;
  grid_list.AppendTo(grid.node);
  blender::bNodeSocket &page = grid.add(false, "Page", blender::SOCK_INT, nullptr);
  blender::bNodeSocket &page_count = grid.add(false, "Page Count", blender::SOCK_INT, nullptr);
  TestNode scroll(tree, "LogicNativeScrollObjectView", "Scroll Object View", 3);
  scroll.node.custom1 = int16_t(LN_UIViewScrollMode::NextPage);
  blender::bNodeSocket &scroll_flow = scroll.flow(true, "Flow");
  scroll.in_object("Viewport Object");
  scroll.add(true, "Item", blender::SOCK_CUSTOM, nullptr, "NodeSocketLogicGeneric");
  scroll.in_int("Lines", 1);
  blender::bNodeSocket &scroll_done = scroll.flow(false, "Done");
  std::array<TestNode *, 3> prints = {};
  TestNode print_accepted(tree, "LogicNativePrint", "Print", 4);
  TestNode print_page(tree, "LogicNativePrint", "Print.001", 5);
  TestNode print_count(tree, "LogicNativePrint", "Print.002", 6);
  prints = {&print_accepted, &print_page, &print_count};
  std::array<blender::bNodeSocket *, 3> sources = {&accepted, &page, &page_count};
  std::array<blender::bNode *, 3> source_nodes = {&on_event.node, &grid.node, &grid.node};
  std::array<blender::bNodeLink, 8> links = {};
  for (size_t index = 0; index < prints.size(); ++index) {
    blender::bNodeSocket &flow = prints[index]->add(true, "Flow", blender::SOCK_BOOLEAN, nullptr);
    blender::bNodeSocket &value = prints[index]->add(true, "Value", blender::SOCK_CUSTOM, nullptr);
    InitLink(links[index * 2], on_event.node, out, prints[index]->node, flow);
    InitLink(links[index * 2 + 1], *source_nodes[index], *sources[index], prints[index]->node, value);
  }
  TestNode print_done(tree, "LogicNativePrint", "Print.003", 7);
  blender::bNodeSocket &done_flow = print_done.add(true, "Flow", blender::SOCK_BOOLEAN, nullptr);
  print_done.in_string("Value", "scrolled");
  InitLink(links[6], on_event.node, out, scroll.node, scroll_flow);
  InitLink(links[7], scroll.node, scroll_done, print_done.node, done_flow);
  for (blender::bNodeLink &link : links) {
    Append(tree.links, link);
  }
  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(program, nullptr);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  std::vector<std::string> validation_errors;
  EXPECT_TRUE(program->ValidateInstructionPayloads(&validation_errors))
      << (validation_errors.empty() ? std::string() : validation_errors.front());
  const std::vector<LN_Instruction> &instructions = program->GetInstructions(LN_Event::OnUpdate);
  const LN_Instruction *scroll_instruction = FindInstruction(instructions,
                                                             LN_OpCode::ScrollObjectView);
  ASSERT_NE(scroll_instruction, nullptr);
  EXPECT_EQ(scroll_instruction->int_value, int32_t(LN_UIViewScrollMode::NextPage));
  EXPECT_NE(scroll_instruction->int_expr_index, LN_INVALID_INDEX);
  EXPECT_NE(scroll_instruction->secondary_value_expr_index, LN_INVALID_INDEX);
  bool accepted_found = false;
  bool page_found = false;
  bool count_found = false;
  bool done_found = false;
  for (const LN_Instruction &instruction : instructions) {
    if (instruction.opcode != LN_OpCode::Print) {
      continue;
    }
    const LN_ValueExpression *value = ValueExpressionAt(*program, instruction.value_expr_index);
    if (value == nullptr) {
      continue;
    }
    if (value->kind == LN_ValueExpressionKind::FromBool) {
      const LN_BoolExpression *read = BoolExpressionAt(*program, value->input0);
      accepted_found = read != nullptr &&
                       read->kind == LN_BoolExpressionKind::UIElementEventAccepted;
    }
    else if (value->kind == LN_ValueExpressionKind::FromInt) {
      const LN_IntExpression *read = IntExpressionAt(*program, value->input0);
      page_found = page_found || (read != nullptr && read->kind == LN_IntExpressionKind::UIViewPage);
      count_found = count_found ||
                    (read != nullptr && read->kind == LN_IntExpressionKind::UIViewPageCount);
    }
    else if (value->kind == LN_ValueExpressionKind::Constant ||
             value->kind == LN_ValueExpressionKind::FromString)
    {
      /* Done runs only after the request was made. */
      const LN_BoolExpression *guard = BoolExpressionAt(*program, instruction.bool_guard_expr_index);
      done_found = done_found || guard != nullptr;
    }
  }
  EXPECT_TRUE(accepted_found);
  EXPECT_TRUE(page_found);
  EXPECT_TRUE(count_found);
  EXPECT_TRUE(done_found);
}

/* Show Message after On UI Event Use: its inputs reach the runtime (a Text read while the game
 * runs), an empty Object is the owner, and Done runs after it. */
TEST(LN_TreeCompiler, ShowMessageCompiles)
{
  blender::bNodeTree tree = {};
  InitTree(tree);
  TestNode on_event(tree, "LogicNativeOnUIEvent", "On UI Event", 1);
  on_event.node.custom1 = int16_t(LN_UIElementEvent::Use);
  on_event.in_string("Action", "bag");
  blender::bNodeSocket &out = on_event.flow(false, "Out");
  TestNode read(tree, "LogicNativeGetGameProperty", "Get Object Property", 2);
  blender::bNodeSocket &message = AddPropertyRead(read, "message");
  TestNode show(tree, "LogicNativeShowMessage", "Show Message", 3);
  blender::bNodeSocket &show_flow = show.flow(true, "Flow");
  show.in_object("Object");
  blender::bNodeSocket &text = show.in_string("Text", "");
  show.in_int("Count", 2);
  show.in_float("Seconds", 2.5f);
  show.in_int("Lines", 3);
  show.in_int("User", 1);
  blender::bNodeSocket &done = show.flow(false, "Done");
  TestNode print(tree, "LogicNativePrint", "Print", 4);
  blender::bNodeSocket &print_flow = print.add(true, "Flow", blender::SOCK_BOOLEAN, nullptr);
  print.in_string("Value", "shown");
  std::array<blender::bNodeLink, 3> links = {};
  InitLink(links[0], on_event.node, out, show.node, show_flow);
  InitLink(links[1], read.node, message, show.node, text);
  InitLink(links[2], show.node, done, print.node, print_flow);
  for (blender::bNodeLink &link : links) {
    Append(tree.links, link);
  }
  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(program, nullptr);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  std::vector<std::string> validation_errors;
  EXPECT_TRUE(program->ValidateInstructionPayloads(&validation_errors))
      << (validation_errors.empty() ? std::string() : validation_errors.front());
  const std::vector<LN_Instruction> &instructions = program->GetInstructions(LN_Event::OnUpdate);
  const LN_Instruction *instruction = FindInstruction(instructions, LN_OpCode::ShowMessage);
  ASSERT_NE(instruction, nullptr);
  EXPECT_EQ(instruction->value_expr_index, LN_INVALID_INDEX);
  const LN_StringExpression *text_read = StringExpressionAt(*program,
                                                            instruction->string_expr_index);
  ASSERT_NE(text_read, nullptr);
  EXPECT_NE(text_read->kind, LN_StringExpressionKind::Constant);
  const LN_FloatExpression *seconds = FloatExpressionAt(*program, instruction->float_expr_index);
  ASSERT_NE(seconds, nullptr);
  EXPECT_FLOAT_EQ(seconds->float_value, 2.5f);
  const LN_IntExpression *lines = IntExpressionAt(*program, instruction->int_expr_index);
  const LN_IntExpression *user = IntExpressionAt(*program, instruction->secondary_int_expr_index);
  ASSERT_NE(lines, nullptr);
  ASSERT_NE(user, nullptr);
  EXPECT_EQ(lines->int_value, 3);
  EXPECT_EQ(user->int_value, 1);
  const LN_IntExpression *count = IntExpressionAt(*program, instruction->tertiary_int_expr_index);
  ASSERT_NE(count, nullptr);
  EXPECT_EQ(count->int_value, 2);
  /* Done: the Print runs only after the message was shown. */
  const LN_Instruction *print_instruction = FindInstruction(instructions, LN_OpCode::Print);
  ASSERT_NE(print_instruction, nullptr);
  EXPECT_NE(BoolExpressionAt(*program, print_instruction->bool_guard_expr_index), nullptr);
}

/* An Object Grid View node with the sockets it had before its list and Moving Items inputs. */
void AddGridViewInputs(TestNode &grid, blender::bNodeSocketValueVector &spacing)
{
  grid.in_object("Viewport Object");
  grid.in_object("Camera");
  grid.add(true, "Slots", blender::SOCK_CUSTOM, nullptr, "NodeSocketLogicGeneric");
  grid.add(true, "Items", blender::SOCK_CUSTOM, nullptr, "NodeSocketLogicGeneric");
  grid.in_int("First Item", 0);
  grid.in_int("Columns", 1);
  grid.in_int("Wheel Rows", 1);
  grid.in_bool("Enabled", true);
  grid.in_bool("Cull Outside", true);
  grid.in_bool("Focus Follow", true);
  grid.in_int("User", 0);
  grid.in_int("Priority", 0);
  grid.in_float("Padding", 0.0f);
  grid.in_bool("Show Counts", true);
  grid.in_bool("Show Names", false);
  grid.in_bool("Show Empty", false);
  grid.in_object("Slot Template");
  grid.in_int("Rows", 0);
  grid.add(true, "Spacing", blender::SOCK_VECTOR, &spacing);
  grid.in_object("Scrollbar");
}

/* A Grid View's list by name and its Inventory reach the runtime, a Property given by a link
 * that changes while the game runs too (and its Filter and Use inputs); On UI Event's Outcome is a
 * runtime text. */
TEST(LN_TreeCompiler, GridViewListInventoryAndOutcomeCompile)
{
  blender::bNodeTree tree = {};
  InitTree(tree);
  TestNode read(tree, "LogicNativeGetGameProperty", "Get Object Property", 1);
  blender::bNodeSocket &list_name = AddPropertyRead(read, "list_name");
  TestNode read_search(tree, "LogicNativeGetGameProperty", "Get Object Property.001", 5);
  blender::bNodeSocket &search_text = AddPropertyRead(read_search, "search");
  TestNode grid(tree, "LogicNativeObjectGridView", "Object Grid View", 2);
  blender::bNodeSocketValueVector no_spacing = {};
  AddGridViewInputs(grid, no_spacing);
  GridListSockets grid_list;
  grid_list.AppendTo(grid.node);
  grid_list.handle_moves.value = true;
  grid_list.drop_in_front.value = true;
  TestNode on_event(tree, "LogicNativeOnUIEvent", "On UI Event", 3);
  on_event.node.custom1 = int16_t(LN_UIElementEvent::DropReceived);
  on_event.in_string("Action", "bag");
  blender::bNodeSocket &out = on_event.flow(false, "Out");
  blender::bNodeSocket &outcome = on_event.add(false, "Outcome", blender::SOCK_STRING, nullptr);
  TestNode print(tree, "LogicNativePrint", "Print", 4);
  blender::bNodeSocket &print_flow = print.add(true, "Flow", blender::SOCK_BOOLEAN, nullptr);
  blender::bNodeSocket &print_value = print.add(true, "Value", blender::SOCK_CUSTOM, nullptr);
  SetCString(grid_list.show_categories.value, "food, weapon");
  SetCString(grid_list.use_action.value, "UseItem");
  SetCString(grid_list.use_keys.value, "Quick1, Quick2");
  std::array<blender::bNodeLink, 4> links = {};
  InitLink(links[0], read.node, list_name, grid.node, grid_list.Input("Property"));
  InitLink(links[1], on_event.node, out, print.node, print_flow);
  InitLink(links[2], on_event.node, outcome, print.node, print_value);
  InitLink(links[3], read_search.node, search_text, grid.node, grid_list.Input("Search"));
  for (blender::bNodeLink &link : links) {
    Append(tree.links, link);
  }

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(program, nullptr);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  EXPECT_FALSE(HasWarningContaining(program->GetCompileReport(), "Items is not used"));
  EXPECT_FALSE(HasWarningContaining(program->GetCompileReport(), "Inventory needs"));
  std::vector<std::string> validation_errors;
  EXPECT_TRUE(program->ValidateInstructionPayloads(&validation_errors))
      << (validation_errors.empty() ? std::string() : validation_errors.front());
  const auto grid_query = std::find_if(
      program->GetQueryExpressions().begin(),
      program->GetQueryExpressions().end(),
      [](const LN_QueryExpression &query) {
        return query.kind == LN_QueryExpressionKind::ObjectGridView;
      });
  ASSERT_NE(grid_query, program->GetQueryExpressions().end());
  ASSERT_EQ(grid_query->input_indices.size(), size_t(LN_GRID_INPUT_COUNT));
  /* Empty Object: whose menu it is, decided while the game runs. */
  EXPECT_EQ(grid_query->input_indices[LN_GRID_INPUT_OBJECT], LN_INVALID_INDEX);
  EXPECT_EQ(grid_query->input_indices[LN_GRID_INPUT_HINT_TEXT], LN_INVALID_INDEX);
  /* The linked Property is read each frame, not folded to the socket's empty text. */
  const uint32_t property = grid_query->input_indices[LN_GRID_INPUT_PROPERTY];
  ASSERT_LT(property, program->GetStringExpressions().size());
  EXPECT_NE(program->GetStringExpressions()[property].kind, LN_StringExpressionKind::Constant);
  for (const LN_GridViewInput input : {LN_GRID_INPUT_HANDLE_MOVES,
                                       LN_GRID_INPUT_SWAP,
                                       LN_GRID_INPUT_DROP_INTO_WORLD,
                                       LN_GRID_INPUT_DROP_AT_POINTER,
                                       LN_GRID_INPUT_DROP_IN_FRONT})
  {
    EXPECT_LT(grid_query->input_indices[input], program->GetBoolExpressions().size());
  }
  EXPECT_LT(grid_query->input_indices[LN_GRID_INPUT_SPLIT_AMOUNT],
            program->GetIntExpressions().size());
  EXPECT_LT(grid_query->input_indices[LN_GRID_INPUT_DISTANCE],
            program->GetFloatExpressions().size());
  EXPECT_LT(grid_query->input_indices[LN_GRID_INPUT_ACCEPT_HINT],
            program->GetStringExpressions().size());
  /* Filter and Use: the texts as typed, a linked Search read each frame. */
  const auto string_at = [&](const LN_GridViewInput input) {
    const LN_StringExpression *expression = StringExpressionAt(*program,
                                                               grid_query->input_indices[input]);
    return expression != nullptr && expression->kind == LN_StringExpressionKind::Constant ?
               expression->string_value :
               std::string(expression != nullptr ? "(runtime)" : "(none)");
  };
  EXPECT_EQ(string_at(LN_GRID_INPUT_SHOW_CATEGORIES), "food, weapon");
  EXPECT_EQ(string_at(LN_GRID_INPUT_SEARCH), "(runtime)");
  EXPECT_EQ(string_at(LN_GRID_INPUT_USE_ACTION), "UseItem");
  EXPECT_EQ(string_at(LN_GRID_INPUT_USE_KEYS), "Quick1, Quick2");
  EXPECT_TRUE(std::any_of(program->GetStringExpressions().begin(),
                          program->GetStringExpressions().end(),
                          [](const LN_StringExpression &expression) {
                            return expression.kind == LN_StringExpressionKind::UIElementEventOutcome;
                          }));
}

/* A grid can change only a list it names: Items and Inventory without a Property are mistakes. */
TEST(LN_TreeCompiler, GridViewWarnsAboutAListItCannotChange)
{
  blender::bNodeTree tree = {};
  InitTree(tree);
  /* Items linked while Property names the list. */
  TestNode read(tree, "LogicNativeGetGameProperty", "Get Object Property", 1);
  blender::bNodeSocket &items = AddPropertyRead(read, "inventory");
  TestNode named(tree, "LogicNativeObjectGridView", "Object Grid View", 2);
  blender::bNodeSocketValueVector no_spacing = {};
  AddGridViewInputs(named, no_spacing);
  GridListSockets named_list;
  named_list.AppendTo(named.node);
  SetCString(named_list.property.value, "inventory");
  blender::bNodeLink link = {};
  blender::bNodeSocket *named_items = nullptr;
  for (blender::bNodeSocket &socket : named.sockets) {
    if (std::strcmp(socket.name, "Items") == 0) {
      named_items = &socket;
    }
  }
  ASSERT_NE(named_items, nullptr);
  InitLink(link, read.node, items, named.node, *named_items);
  Append(tree.links, link);
  const std::shared_ptr<LN_Program> named_program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(named_program, nullptr);
  EXPECT_FALSE(named_program->GetCompileReport().HasErrors());
  EXPECT_TRUE(HasWarningContaining(named_program->GetCompileReport(), "Items is not used"));
  EXPECT_FALSE(HasWarningContaining(named_program->GetCompileReport(), "Inventory needs"));

  /* Inventory on without a Property. */
  blender::bNodeTree other_tree = {};
  InitTree(other_tree);
  TestNode unnamed(other_tree, "LogicNativeObjectGridView", "Object Grid View", 1);
  AddGridViewInputs(unnamed, no_spacing);
  GridListSockets unnamed_list;
  unnamed_list.AppendTo(unnamed.node);
  unnamed_list.handle_moves.value = true;
  const std::shared_ptr<LN_Program> unnamed_program = LN_TreeCompiler().Compile(other_tree);
  ASSERT_NE(unnamed_program, nullptr);
  EXPECT_FALSE(unnamed_program->GetCompileReport().HasErrors());
  EXPECT_TRUE(HasWarningContaining(unnamed_program->GetCompileReport(), "Inventory needs"));
}


/* A number linked into a text input reads as its text: "Page {} / {}" with an Int and a Float. */
TEST(LN_TreeCompiler, NumbersLinkedIntoTextInputsReadAsText)
{
  blender::bNodeTree tree = {};
  InitTree(tree);
  TestNode tick(tree, "LogicNativeOnFixedUpdate", "On Logic + Physics Step", 1);
  blender::bNodeSocket &out = tick.flow(false, "Out");
  TestNode page(tree, "LogicNativeValueInt", "Integer", 2);
  blender::bNodeSocketValueInt page_value = {};
  page_value.value = 2;
  blender::bNodeSocket &page_out = page.add(false, "Int", blender::SOCK_INT, &page_value);
  TestNode count(tree, "LogicNativeValueFloat", "Float", 3);
  blender::bNodeSocketValueFloat count_value = {};
  count_value.value = 1.5f;
  blender::bNodeSocket &count_out = count.add(false, "Float", blender::SOCK_FLOAT, &count_value);
  TestNode format(tree, "LogicNativeFormattedString", "Formatted String", 4);
  format.in_string("Format String", "Page {} / {}");
  blender::bNodeSocket &a = format.in_string("A", "");
  blender::bNodeSocket &b = format.in_string("B", "");
  format.in_string("C", "");
  format.in_string("D", "");
  blender::bNodeSocket &text = format.add(false, "String", blender::SOCK_STRING, nullptr);
  TestNode print(tree, "LogicNativePrint", "Print", 5);
  blender::bNodeSocket &print_flow = print.add(true, "Flow", blender::SOCK_BOOLEAN, nullptr);
  blender::bNodeSocket &print_value = print.add(true, "Value", blender::SOCK_CUSTOM, nullptr);
  std::array<blender::bNodeLink, 4> links = {};
  InitLink(links[0], tick.node, out, print.node, print_flow);
  InitLink(links[1], page.node, page_out, format.node, a);
  InitLink(links[2], count.node, count_out, format.node, b);
  InitLink(links[3], format.node, text, print.node, print_value);
  for (blender::bNodeLink &link : links) {
    Append(tree.links, link);
  }
  /* A number a node builds at runtime (here a game property read) too. */
  TestNode speed(tree, "LogicNativeGetGamePropertyFloat", "Get Object Property (Float)", 6);
  speed.in_object("Object");
  speed.in_string("Property", "speed");
  blender::bNodeSocket &speed_value = speed.add(false, "Property Value", blender::SOCK_FLOAT, nullptr);
  SetCString(speed_value.identifier, "Value");
  TestNode speed_format(tree, "LogicNativeFormattedString", "Formatted String.001", 7);
  speed_format.in_string("Format String", "Speed {}");
  blender::bNodeSocket &speed_a = speed_format.in_string("A", "");
  speed_format.in_string("B", "");
  speed_format.in_string("C", "");
  speed_format.in_string("D", "");
  blender::bNodeSocket &speed_text = speed_format.add(false, "String", blender::SOCK_STRING, nullptr);
  TestNode speed_print(tree, "LogicNativePrint", "Print.001", 8);
  blender::bNodeSocket &speed_flow = speed_print.add(true, "Flow", blender::SOCK_BOOLEAN, nullptr);
  blender::bNodeSocket &speed_shown = speed_print.add(true, "Value", blender::SOCK_CUSTOM, nullptr);
  std::array<blender::bNodeLink, 3> speed_links = {};
  InitLink(speed_links[0], tick.node, out, speed_print.node, speed_flow);
  InitLink(speed_links[1], speed.node, speed_value, speed_format.node, speed_a);
  InitLink(speed_links[2], speed_format.node, speed_text, speed_print.node, speed_shown);
  for (blender::bNodeLink &link : speed_links) {
    Append(tree.links, link);
  }

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(program, nullptr);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  std::vector<std::string> validation_errors;
  ASSERT_TRUE(program->ValidateInstructionPayloads(&validation_errors))
      << (validation_errors.empty() ? std::string() : validation_errors.front());
  /* The read number becomes text through its value, not a number expression read as text. */
  bool converted = false;
  for (const LN_StringExpression &expression : program->GetStringExpressions()) {
    if (expression.kind != LN_StringExpressionKind::Format) {
      continue;
    }
    const LN_StringExpression *a_text = StringExpressionAt(*program, expression.input1);
    if (a_text == nullptr || a_text->kind != LN_StringExpressionKind::FromGenericValue) {
      continue;
    }
    const LN_ValueExpression *value = ValueExpressionAt(*program, a_text->value_expr_index);
    if (value != nullptr && value->kind == LN_ValueExpressionKind::FromFloat) {
      const LN_FloatExpression *read = FloatExpressionAt(*program, value->input0);
      converted = converted || (read != nullptr &&
                                read->kind == LN_FloatExpressionKind::SnapshotGameProperty);
    }
  }
  EXPECT_TRUE(converted);
  /* Only the constant line is checked when run: the test object has no properties. */
  EXPECT_GE(CountPrintsOfOneFixedStep(program, 1, "Page 2 / 1.5"), 1u);
}


/* Open Object Menu's Pause Game and Toggle reach the command, Closed runs only when a toggle
 * closed the menu, and a tree started by Input Action needs no other root. A key that only opens
 * menus is read in the frame update (with Pause Game and Toggle also while paused); one whose
 * menus lead on, or that also starts game logic, keeps its Logic + Physics steps. */
TEST(LN_TreeCompiler, OpenObjectMenuPauseToggleAndClosedCompile)
{
  blender::bNodeTree tree = {};
  InitTree(tree);
  TestNode pressed(tree, "LogicNativeInputAction", "Input Action", 1);
  pressed.in_string("Action", "Pause");
  pressed.in_int("User", 0);
  blender::bNodeSocket &out = pressed.flow(false, "Out");
  TestNode open(tree, "LogicNativeOpenObjectMenu", "Open Object Menu", 2);
  open.node.custom2 = int16_t(LN_PackUIMenuNodeSettings(LN_UIMenuOpenMode::Push,
                                                        LN_UIMenuPresentation::Screen,
                                                        LN_UIMenuBackBehavior::CloseMenu) |
                              LN_UI_MENU_NODE_PAUSES_GAME | LN_UI_MENU_NODE_TOGGLES);
  blender::bNodeSocket &open_flow = open.flow(true, "Flow");
  open.in_object("Camera");
  blender::bNodeSocketValueCollection no_collection = {};
  open.add(true, "Collection", blender::SOCK_COLLECTION, &no_collection);
  open.in_string("Input Context", "");
  open.in_int("User", 0);
  open.flow(false, "Opened");
  open.flow(false, "Failed");
  blender::bNodeSocket &closed = open.flow(false, "Closed");
  open.add(false, "Menu", blender::SOCK_INT, nullptr);
  open.add(false, "Result", blender::SOCK_INT, nullptr);
  TestNode print(tree, "LogicNativePrint", "Print", 3);
  blender::bNodeSocket &print_flow = print.add(true, "Flow", blender::SOCK_BOOLEAN, nullptr);
  print.in_string("Value", "closed");
  std::array<blender::bNodeLink, 2> links = {};
  InitLink(links[0], pressed.node, out, open.node, open_flow);
  InitLink(links[1], open.node, closed, print.node, print_flow);
  for (blender::bNodeLink &link : links) {
    Append(tree.links, link);
  }
  const auto find_open = [](const LN_Program &program, const LN_Event event) {
    return FindInstruction(program.GetInstructions(event), LN_OpCode::OpenObjectMenu);
  };
  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(program, nullptr);
  const LN_CompileReport &report = program->GetCompileReport();
  EXPECT_FALSE(report.HasErrors()) << FormatCompileReport(report);
  EXPECT_FALSE(HasWarningContaining(report, "has no On Init")) << FormatCompileReport(report);
  std::vector<std::string> validation_errors;
  EXPECT_TRUE(program->ValidateInstructionPayloads(&validation_errors))
      << (validation_errors.empty() ? std::string() : validation_errors.front());
  /* Closed leads on (to a node that may need Logic + Physics), so the key stays on those steps,
   * which stop while paused: a warning says only Back closes the menu then. */
  const LN_Instruction *open_instruction = find_open(*program, LN_Event::OnFixedUpdate);
  ASSERT_NE(open_instruction, nullptr) << FormatCompileReport(report);
  EXPECT_TRUE(HasWarningContaining(report, "only Back closes the menu"))
      << FormatCompileReport(report);
  EXPECT_TRUE(LN_UnpackUIMenuPausesGame(open_instruction->secondary_int_value));
  EXPECT_TRUE(LN_UnpackUIMenuToggles(open_instruction->secondary_int_value));
  EXPECT_EQ(LN_UnpackUIMenuOpenMode(open_instruction->secondary_int_value),
            LN_UIMenuOpenMode::Push);
  EXPECT_EQ(LN_UnpackUIMenuBackBehavior(open_instruction->secondary_int_value),
            LN_UIMenuBackBehavior::CloseMenu);
  /* The Print after Closed runs only on the toggle's Closed result. */
  const LN_Instruction *print_instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::Print);
  ASSERT_NE(print_instruction, nullptr);
  const LN_BoolExpression *guard = BoolExpressionAt(*program,
                                                    print_instruction->bool_guard_expr_index);
  ASSERT_NE(guard, nullptr);
  EXPECT_EQ(guard->kind, LN_BoolExpressionKind::UICommandClosed);
  EXPECT_EQ(program->GetBoolExpressions()[guard->input0].kind,
            LN_BoolExpressionKind::UICommandCompleted);

  /* Nothing after the menu: the key is read in the frame update, where the UI also reads Back,
   * and, for this pause menu with Toggle, also while paused. */
  Remove(tree.links, links[1]);
  const std::shared_ptr<LN_Program> direct = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(direct, nullptr);
  EXPECT_FALSE(direct->GetCompileReport().HasErrors());
  EXPECT_FALSE(HasWarningContaining(direct->GetCompileReport(), "only Back closes the menu"));
  const LN_Instruction *direct_open = find_open(*direct, LN_Event::OnUpdate);
  ASSERT_NE(direct_open, nullptr) << FormatCompileReport(direct->GetCompileReport());
  EXPECT_EQ(find_open(*direct, LN_Event::OnFixedUpdate), nullptr);
  EXPECT_NE(direct_open->flags & LN_INSTRUCTION_FLAG_UI_EVENT_ROUTE, 0u);
  EXPECT_TRUE(direct->HasUIEventRoutes());

  /* A toggle that does not pause stays a game route: a paused game does not open it. It is still
   * read in the frame update. */
  open.node.custom2 &= int16_t(~LN_UI_MENU_NODE_PAUSES_GAME);
  const std::shared_ptr<LN_Program> no_pause = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(no_pause, nullptr);
  const LN_Instruction *game_route = find_open(*no_pause, LN_Event::OnUpdate);
  ASSERT_NE(game_route, nullptr);
  EXPECT_EQ(game_route->flags & LN_INSTRUCTION_FLAG_UI_EVENT_ROUTE, 0u);
  EXPECT_FALSE(no_pause->HasUIEventRoutes());
  open.node.custom2 |= LN_UI_MENU_NODE_PAUSES_GAME;

  /* The same Input Action also starting game logic keeps its Logic + Physics steps. */
  TestNode other(tree, "LogicNativePrint", "Print.001", 4);
  blender::bNodeSocket &other_flow = other.add(true, "Flow", blender::SOCK_BOOLEAN, nullptr);
  other.in_string("Value", "pressed");
  blender::bNodeLink to_other = {};
  InitLink(to_other, pressed.node, out, other.node, other_flow);
  Append(tree.links, to_other);
  const std::shared_ptr<LN_Program> shared = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(shared, nullptr);
  EXPECT_FALSE(shared->GetCompileReport().HasErrors())
      << FormatCompileReport(shared->GetCompileReport());
  EXPECT_NE(find_open(*shared, LN_Event::OnFixedUpdate), nullptr);
  EXPECT_EQ(find_open(*shared, LN_Event::OnUpdate), nullptr);
  Remove(tree.links, to_other);

  /* A Keyboard Key straight into the menu is read in the frame update too. */
  TestNode key(tree, "LogicNativeKeyboardKey", "Keyboard Key", 5);
  key.in_string("Key", "ESC");
  blender::bNodeSocket &key_out = key.flow(false, "Out");
  Remove(tree.links, links[0]);
  blender::bNodeLink from_key = {};
  InitLink(from_key, key.node, key_out, open.node, open_flow);
  Append(tree.links, from_key);
  const std::shared_ptr<LN_Program> keyboard = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(keyboard, nullptr);
  EXPECT_FALSE(keyboard->GetCompileReport().HasErrors())
      << FormatCompileReport(keyboard->GetCompileReport());
  EXPECT_NE(find_open(*keyboard, LN_Event::OnUpdate), nullptr);
  EXPECT_TRUE(keyboard->HasUIEventRoutes());

  /* A Mouse Button straight into the menu too (a right click opens and closes it). */
  TestNode mouse(tree, "LogicNativeMouseButton", "Mouse Button", 6);
  mouse.in_string("Button", "RIGHTMOUSE");
  blender::bNodeSocket &mouse_out = mouse.flow(false, "Out");
  Remove(tree.links, from_key);
  blender::bNodeLink from_mouse = {};
  InitLink(from_mouse, mouse.node, mouse_out, open.node, open_flow);
  Append(tree.links, from_mouse);
  const std::shared_ptr<LN_Program> mouse_program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(mouse_program, nullptr);
  EXPECT_FALSE(mouse_program->GetCompileReport().HasErrors())
      << FormatCompileReport(mouse_program->GetCompileReport());
  EXPECT_NE(find_open(*mouse_program, LN_Event::OnUpdate), nullptr);
  EXPECT_EQ(find_open(*mouse_program, LN_Event::OnFixedUpdate), nullptr);
  EXPECT_TRUE(mouse_program->HasUIEventRoutes());

  /* A tree with no nodes (one waiting for them) gets no hint about roots. */
  blender::bNodeTree empty = {};
  InitTree(empty);
  const std::shared_ptr<LN_Program> empty_program = LN_TreeCompiler().Compile(empty);
  ASSERT_NE(empty_program, nullptr);
  EXPECT_FALSE(HasWarningContaining(empty_program->GetCompileReport(), "has no On Init"));
}

/** The Print messages of each of \a steps Logic + Physics steps of a compiled tree. */
std::vector<std::vector<std::string>> PrintsOfFixedSteps(
    const std::shared_ptr<LN_Program> &program, const int steps)
{
  LN_RuntimeTree runtime_tree(program, reinterpret_cast<KX_GameObject *>(uintptr_t(64)), 0, 0);
  std::vector<std::vector<std::string>> prints;
  for (int step = 1; step <= steps; step++) {
    LN_TickContext context;
    context.event = LN_Event::OnFixedUpdate;
    context.tick_index = uint64_t(step);
    LN_CommandBuffer command_buffer;
    command_buffer.BeginRecording();
    runtime_tree.ExecuteReady(command_buffer, context);
    command_buffer.EndRecording();
    std::vector<std::string> &messages = prints.emplace_back();
    for (const LN_CommandBuffer::Command &command : command_buffer.TakeRecordedCommands()) {
      if (command.type == LN_CommandBuffer::CommandType::Print) {
        messages.push_back(command.property_name);
      }
    }
  }
  return prints;
}

/* Move To's Reached leads on (linking it was a compile error), and only then does Move To read
 * where the object is. */
TEST(LN_TreeCompiler, MoveToReachedReadsThePositionOnlyWhenLinked)
{
  for (const bool link_reached : {false, true}) {
    SCOPED_TRACE(link_reached ? "Reached linked" : "Reached not linked");
    blender::bNodeTree tree = {};
    InitTree(tree);
    TestNode step(tree, "LogicNativeOnFixedUpdate", "On Logic + Physics Step", 1);
    blender::bNodeSocket &tick = step.flow(false, "Out");
    TestNode move(tree, "LogicNativeMoveToward", "Move To", 2);
    blender::bNodeSocket &move_flow = move.flow(true, "Flow");
    move.in_object("Object");
    blender::bNodeSocketValueVector target = {};
    target.value[0] = 3.0f;
    move.add(true, "Target Position", blender::SOCK_VECTOR, &target);
    move.in_float("Speed", 5.0f);
    move.in_float("Stop Distance", 0.5f);
    move.flow(false, "Done");
    blender::bNodeSocket &reached = move.flow(false, "Reached");
    TestNode print(tree, "LogicNativePrint", "Print", 3);
    blender::bNodeSocket &print_flow = print.flow(true, "Flow");
    print.in_string("Value", "reached");
    blender::bNodeLink to_move = {};
    InitLink(to_move, step.node, tick, move.node, move_flow);
    Append(tree.links, to_move);
    blender::bNodeLink to_print = {};
    if (link_reached) {
      InitLink(to_print, move.node, reached, print.node, print_flow);
      Append(tree.links, to_print);
    }
    const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
    ASSERT_NE(program, nullptr);
    EXPECT_FALSE(program->GetCompileReport().HasErrors())
        << FormatCompileReport(program->GetCompileReport());
    const std::vector<LN_Instruction> &instructions = program->GetInstructions(
        LN_Event::OnFixedUpdate);
    const LN_Instruction *move_instruction = FindInstruction(instructions, LN_OpCode::MoveToward);
    ASSERT_NE(move_instruction, nullptr);
    EXPECT_EQ(move_instruction->secondary_vector_expr_index != LN_INVALID_INDEX, link_reached);
    EXPECT_EQ(FindInstruction(instructions, LN_OpCode::Print) != nullptr, link_reached);
  }
}

/* A number linked into String Switch Flow is compared as text: 2 picks the case "2". */
TEST(LN_TreeCompiler, StringSwitchFlowComparesALinkedNumberAsText)
{
  blender::bNodeTree tree = {};
  InitTree(tree);
  TestNode step(tree, "LogicNativeOnFixedUpdate", "On Logic + Physics Step", 1);
  blender::bNodeSocket &tick = step.flow(false, "Out");
  TestNode number(tree, "LogicNativeValueInt", "Integer", 2);
  blender::bNodeSocketValueInt two = {};
  two.value = 2;
  blender::bNodeSocket &number_out = number.add(false, "Int", blender::SOCK_INT, &two);
  TestNode choice(tree, "LogicNativeSwitchFlowString", "String Switch", 3);
  blender::bNodeSocket &choice_flow = choice.flow(true, "Flow");
  blender::bNodeSocket &choice_value = choice.in_string("Value", "");
  choice.in_string("Case 0", "1");
  choice.in_string("Case 1", "2");
  blender::bNodeSocket *outputs[] = {&choice.flow(false, "Then 0"),
                                     &choice.flow(false, "Then 1"),
                                     &choice.flow(false, "Default")};
  const char *messages[] = {"one", "two", "default"};
  std::deque<TestNode> prints;
  std::deque<blender::bNodeLink> links;
  InitLink(links.emplace_back(), step.node, tick, choice.node, choice_flow);
  InitLink(links.emplace_back(), number.node, number_out, choice.node, choice_value);
  for (int index = 0; index < 3; index++) {
    TestNode &print = prints.emplace_back(tree, "LogicNativePrint", messages[index], 4 + index);
    blender::bNodeSocket &print_flow = print.flow(true, "Flow");
    print.in_string("Value", messages[index]);
    InitLink(links.emplace_back(), choice.node, *outputs[index], print.node, print_flow);
  }
  for (blender::bNodeLink &link : links) {
    Append(tree.links, link);
  }
  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(program, nullptr);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  const std::vector<std::vector<std::string>> prints_per_step = PrintsOfFixedSteps(program, 1);
  EXPECT_EQ(prints_per_step.back(), std::vector<std::string>{"two"});
}

/* Make List holds only the items that are linked. */
TEST(LN_TreeCompiler, MakeListHoldsOnlyLinkedItems)
{
  blender::bNodeTree tree = {};
  InitTree(tree);
  TestNode step(tree, "LogicNativeOnFixedUpdate", "On Logic + Physics Step", 1);
  blender::bNodeSocket &tick = step.flow(false, "Out");
  TestNode number(tree, "LogicNativeValueInt", "Integer", 2);
  blender::bNodeSocketValueInt five = {};
  five.value = 5;
  blender::bNodeSocket &number_out = number.add(false, "Int", blender::SOCK_INT, &five);
  TestNode make(tree, "LogicNativeMakeList", "Make List", 3);
  blender::bNodeSocket &item_a = make.add(
      true, "Item A", blender::SOCK_CUSTOM, nullptr, "NodeSocketLogicGeneric");
  make.add(true, "Item B", blender::SOCK_CUSTOM, nullptr, "NodeSocketLogicGeneric");
  make.add(true, "Item C", blender::SOCK_CUSTOM, nullptr, "NodeSocketLogicGeneric");
  blender::bNodeSocket &list = make.add(
      false, "List", blender::SOCK_CUSTOM, nullptr, "NodeSocketLogicList");
  TestNode length(tree, "LogicNativeListLength", "List Length", 4);
  blender::bNodeSocket &length_list = length.add(
      true, "List", blender::SOCK_CUSTOM, nullptr, "NodeSocketLogicList");
  blender::bNodeSocket &length_out = length.add(false, "Length", blender::SOCK_INT, nullptr);
  TestNode print(tree, "LogicNativePrint", "Print", 5);
  blender::bNodeSocket &print_flow = print.flow(true, "Flow");
  blender::bNodeSocket &print_value = print.in_string("Value", "");
  std::deque<blender::bNodeLink> links;
  InitLink(links.emplace_back(), step.node, tick, print.node, print_flow);
  InitLink(links.emplace_back(), number.node, number_out, make.node, item_a);
  InitLink(links.emplace_back(), make.node, list, length.node, length_list);
  InitLink(links.emplace_back(), length.node, length_out, print.node, print_value);
  for (blender::bNodeLink &link : links) {
    Append(tree.links, link);
  }
  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(program, nullptr);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  EXPECT_EQ(PrintsOfFixedSteps(program, 1).back(), std::vector<std::string>{"1"});
}

/* Store Value keeps what its Value was when its Flow ran (On Init here), and does not store
 * again on later steps. */
TEST(LN_TreeCompiler, StoreValueStoresOnlyWhenItsFlowRuns)
{
  blender::bNodeTree tree = {};
  InitTree(tree);
  TestNode init(tree, "LogicNativeOnInit", "On Init", 1);
  blender::bNodeSocket &init_out = init.flow(false, "Out");
  TestNode step(tree, "LogicNativeOnFixedUpdate", "On Logic + Physics Step", 2);
  blender::bNodeSocket &tick = step.flow(false, "Out");
  /* A value that changes on every step. */
  TestNode random(tree, "LogicNativeRandomFloat", "Random Float", 3);
  random.in_float("Min", 0.0f);
  random.in_float("Max", 1000000.0f);
  blender::bNodeSocket &random_out = random.add(false, "Value", blender::SOCK_FLOAT, nullptr);
  TestNode store(tree, "LogicNativeStoreValue", "Store Value", 4);
  store.node.custom1 = 1; /* Initialize off: only the Flow stores. */
  blender::bNodeSocket &store_flow = store.flow(true, "Flow");
  blender::bNodeSocket &store_value = store.in_float("Value", 0.0f);
  store.flow(false, "Done");
  blender::bNodeSocket &stored = store.add(false, "Value", blender::SOCK_FLOAT, nullptr);
  TestNode print(tree, "LogicNativePrint", "Print", 5);
  blender::bNodeSocket &print_flow = print.flow(true, "Flow");
  blender::bNodeSocket &print_value = print.in_string("Value", "");
  std::deque<blender::bNodeLink> links;
  InitLink(links.emplace_back(), init.node, init_out, store.node, store_flow);
  InitLink(links.emplace_back(), random.node, random_out, store.node, store_value);
  InitLink(links.emplace_back(), step.node, tick, print.node, print_flow);
  InitLink(links.emplace_back(), store.node, stored, print.node, print_value);
  for (blender::bNodeLink &link : links) {
    Append(tree.links, link);
  }
  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(program, nullptr);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  const std::vector<std::vector<std::string>> prints = PrintsOfFixedSteps(program, 4);
  ASSERT_EQ(prints.size(), 4u);
  for (const std::vector<std::string> &messages : prints) {
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages[0], prints[0][0]);
  }
}

/* Split screen: Set Camera's User, read while the game runs, and an empty Camera (no camera). */
TEST(LN_TreeCompiler, SetCameraTakesAUserReadWhileTheGameRuns)
{
  blender::bNodeTree tree = {};
  InitTree(tree);
  TestNode on_update(tree, "LogicNativeOnUpdate", "On Update", 1);
  blender::bNodeSocket &tick = on_update.flow(false, "Out");
  TestNode player(tree, "LogicNativeGetGameProperty", "Get Object Property", 2);
  blender::bNodeSocket &player_value = AddPropertyRead(player, "player");
  TestNode set_camera(tree, "LogicNativeSetCamera", "Set Camera", 3);
  blender::bNodeSocket &set_flow = set_camera.flow(true, "Flow");
  set_camera.in_object("Camera");
  blender::bNodeSocket &user = set_camera.in_int("User", 0);
  set_camera.flow(false, "Out");
  std::deque<blender::bNodeLink> links;
  InitLink(links.emplace_back(), on_update.node, tick, set_camera.node, set_flow);
  InitLink(links.emplace_back(), player.node, player_value, set_camera.node, user);
  for (blender::bNodeLink &link : links) {
    Append(tree.links, link);
  }

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(program, nullptr);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  std::vector<std::string> validation_errors;
  EXPECT_TRUE(program->ValidateInstructionPayloads(&validation_errors))
      << (validation_errors.empty() ? std::string() : validation_errors.front());
  const LN_Instruction *instruction = FindInstruction(program->GetInstructions(LN_Event::OnUpdate),
                                                      LN_OpCode::SetActiveCamera);
  ASSERT_NE(instruction, nullptr);
  /* No camera: players 1 to 7 lose their view; player 0 keeps the active camera. */
  EXPECT_EQ(instruction->value_expr_index, LN_INVALID_INDEX);
  const LN_IntExpression *user_expression = IntExpressionAt(*program,
                                                            instruction->int_expr_index);
  ASSERT_NE(user_expression, nullptr);
  EXPECT_EQ(user_expression->kind, LN_IntExpressionKind::FromGenericValue);
}

/* The game used to reject this tree: the active camera read was missing from its scheduler
 * summary when nothing else read the snapshot. */
TEST(LN_TreeCompiler, AnEmptyCameraSocketReadsTheActiveCameraInAValidProgram)
{
  blender::bNodeTree tree = {};
  InitTree(tree);
  TestNode on_init(tree, "LogicNativeOnInit", "On Init", 1);
  blender::bNodeSocket &init_out = on_init.flow(false, "Out");
  TestNode set_fov(tree, "LogicNativeSetCameraFov", "Set FOV", 2);
  blender::bNodeSocket &fov_flow = set_fov.flow(true, "Flow");
  set_fov.in_object("Camera");
  set_fov.in_float("FOV", 70.0f);
  set_fov.flow(false, "Out");
  blender::bNodeLink link = {};
  InitLink(link, on_init.node, init_out, set_fov.node, fov_flow);
  Append(tree.links, link);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(program, nullptr);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  std::vector<std::string> validation_errors;
  EXPECT_TRUE(program->ValidateInstructionPayloads(&validation_errors))
      << (validation_errors.empty() ? std::string() : validation_errors.front());
}

TEST(LN_TreeCompiler, SetSplitScreenAndSetCameraViewportReadTheirInputsWhileTheGameRuns)
{
  blender::bNodeTree tree = {};
  InitTree(tree);
  TestNode on_update(tree, "LogicNativeOnUpdate", "On Update", 1);
  blender::bNodeSocket &tick = on_update.flow(false, "Out");
  TestNode split_on(tree, "LogicNativeGetGameProperty", "Get Object Property", 2);
  blender::bNodeSocket &split_on_value = AddPropertyRead(split_on, "split");
  TestNode map_left(tree, "LogicNativeGetGameProperty", "Get Object Property.001", 3);
  blender::bNodeSocket &map_left_value = AddPropertyRead(map_left, "map_left");

  TestNode split(tree, "LogicNativeSetSplitScreen", "Set Split Screen", 4);
  /* Top and Bottom (2) and Favor Bottom (4) in custom1, Keep Horizontal in custom2. */
  split.node.custom1 = 2 | 4;
  split.node.custom2 = 3;
  blender::bNodeSocket &split_flow = split.flow(true, "Flow");
  blender::bNodeSocket &enabled = split.in_bool("Enabled", true);
  split.in_int("Divider", 4);
  blender::bNodeSocket &keep_mouse = split.in_bool("Keep Mouse in View", true);
  /* Blender names the visible output Done (its identifier is Out). */
  blender::bNodeSocket &split_out = split.flow(false, "Done");

  TestNode viewport(tree, "LogicNativeSetCameraViewport", "Set Camera Viewport", 5);
  /* Area Player's View: a minimap in the view of the player read while the game runs. */
  viewport.node.custom1 = 1;
  blender::bNodeSocket &viewport_flow = viewport.flow(true, "Flow");
  viewport.in_object("Camera");
  viewport.in_bool("Enabled", true);
  blender::bNodeSocket &viewport_user = viewport.in_int("User", 0);
  blender::bNodeSocket &left = viewport.in_float("Left", 0.7f);
  viewport.in_float("Top", 0.05f);
  viewport.in_float("Width", 0.25f);
  viewport.in_float("Height", 0.3f);
  viewport.in_bool("On Top", false);
  viewport.flow(false, "Out");

  std::deque<blender::bNodeLink> links;
  InitLink(links.emplace_back(), on_update.node, tick, split.node, split_flow);
  InitLink(links.emplace_back(), split_on.node, split_on_value, split.node, enabled);
  InitLink(links.emplace_back(), split_on.node, split_on_value, split.node, keep_mouse);
  InitLink(links.emplace_back(), split.node, split_out, viewport.node, viewport_flow);
  InitLink(links.emplace_back(), map_left.node, map_left_value, viewport.node, left);
  InitLink(links.emplace_back(), split_on.node, split_on_value, viewport.node, viewport_user);
  for (blender::bNodeLink &link : links) {
    Append(tree.links, link);
  }

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(program, nullptr);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  std::vector<std::string> validation_errors;
  EXPECT_TRUE(program->ValidateInstructionPayloads(&validation_errors))
      << (validation_errors.empty() ? std::string() : validation_errors.front());
  const std::vector<LN_Instruction> &instructions = program->GetInstructions(LN_Event::OnUpdate);

  const LN_Instruction *set_split = FindInstruction(instructions, LN_OpCode::SetSplitScreen);
  ASSERT_NE(set_split, nullptr);
  EXPECT_EQ(set_split->int_value & LN_SPLIT_SCREEN_TWO_PLAYERS_MASK, 2);
  EXPECT_EQ((set_split->int_value >> LN_SPLIT_SCREEN_THREE_PLAYERS_SHIFT) & 1, 1);
  /* Keep Mouse in View is read while the game runs; the command adds FREE_MOUSE when off. */
  EXPECT_EQ(set_split->int_value & LN_SPLIT_SCREEN_FREE_MOUSE, 0);
  const LN_BoolExpression *split_keep_mouse = BoolExpressionAt(
      *program, set_split->secondary_bool_expr_index);
  ASSERT_NE(split_keep_mouse, nullptr);
  EXPECT_NE(split_keep_mouse->kind, LN_BoolExpressionKind::Constant);
  EXPECT_EQ(set_split->int_value >> LN_SPLIT_SCREEN_FIELD_OF_VIEW_SHIFT, 3);
  const LN_BoolExpression *split_enabled = BoolExpressionAt(*program, set_split->bool_expr_index);
  ASSERT_NE(split_enabled, nullptr);
  EXPECT_NE(split_enabled->kind, LN_BoolExpressionKind::Constant);
  const LN_IntExpression *divider = IntExpressionAt(*program, set_split->int_expr_index);
  ASSERT_NE(divider, nullptr);
  EXPECT_EQ(divider->kind, LN_IntExpressionKind::Constant);
  EXPECT_EQ(divider->int_value, 4);

  const LN_Instruction *set_viewport = FindInstruction(instructions,
                                                       LN_OpCode::SetCameraViewport);
  ASSERT_NE(set_viewport, nullptr);
  /* An empty Camera is the active camera, as for Set FOV. */
  const LN_ValueExpression *camera = ValueExpressionAt(*program, set_viewport->value_expr_index);
  ASSERT_NE(camera, nullptr);
  EXPECT_EQ(camera->kind, LN_ValueExpressionKind::ActiveCamera);
  EXPECT_EQ(set_viewport->secondary_int_value, 1);
  const LN_IntExpression *viewport_player = IntExpressionAt(*program,
                                                           set_viewport->int_expr_index);
  ASSERT_NE(viewport_player, nullptr);
  EXPECT_NE(viewport_player->kind, LN_IntExpressionKind::Constant);
  const LN_FloatExpression *left_expression = FloatExpressionAt(*program,
                                                                set_viewport->float_expr_index);
  ASSERT_NE(left_expression, nullptr);
  EXPECT_NE(left_expression->kind, LN_FloatExpressionKind::Constant);
  const LN_FloatExpression *height = FloatExpressionAt(
      *program, set_viewport->quaternary_float_expr_index);
  ASSERT_NE(height, nullptr);
  EXPECT_FLOAT_EQ(height->float_value, 0.3f);
  const LN_BoolExpression *on_top = BoolExpressionAt(*program,
                                                     set_viewport->secondary_bool_expr_index);
  ASSERT_NE(on_top, nullptr);
  EXPECT_FALSE(on_top->bool_value);
}

TEST(LN_TreeCompiler, GetPlayerViewOutputsReachTheirReaders)
{
  blender::bNodeTree tree = {};
  InitTree(tree);
  TestNode on_update(tree, "LogicNativeOnUpdate", "On Update", 1);
  blender::bNodeSocket &tick = on_update.flow(false, "Out");
  TestNode player(tree, "LogicNativeGetGameProperty", "Get Object Property", 2);
  blender::bNodeSocket &player_value = AddPropertyRead(player, "player");
  TestNode view(tree, "LogicNativeGetPlayerView", "Get Player View", 3);
  blender::bNodeSocket &view_user = view.in_int("User", 0);
  blender::bNodeSocket &view_camera = view.add(false, "Camera", blender::SOCK_OBJECT, nullptr);
  blender::bNodeSocket &shown = view.add(false, "Shown", blender::SOCK_BOOLEAN, nullptr);
  view.add(false, "Left", blender::SOCK_FLOAT, nullptr);
  blender::bNodeSocket &top = view.add(false, "Top", blender::SOCK_FLOAT, nullptr);
  view.add(false, "Width", blender::SOCK_FLOAT, nullptr);
  view.add(false, "Height", blender::SOCK_FLOAT, nullptr);
  blender::bNodeSocket &views = view.add(false, "Views", blender::SOCK_INT, nullptr);
  TestNode branch(tree, "LogicNativeBranch", "Branch", 4);
  blender::bNodeSocket &branch_flow = branch.flow(true, "Flow");
  blender::bNodeSocket &condition = branch.in_bool("Condition", false);
  blender::bNodeSocket &branch_true = branch.flow(false, "True");
  branch.flow(false, "False");
  TestNode print_top(tree, "LogicNativePrint", "Print", 5);
  blender::bNodeSocket &print_top_flow = print_top.flow(true, "Flow");
  blender::bNodeSocket &print_top_value = print_top.in_string("Value", "");
  TestNode print_views(tree, "LogicNativePrint", "Print.001", 6);
  blender::bNodeSocket &print_views_flow = print_views.flow(true, "Flow");
  blender::bNodeSocket &print_views_value = print_views.in_string("Value", "");
  TestNode set_camera(tree, "LogicNativeSetCamera", "Set Camera", 7);
  blender::bNodeSocket &set_flow = set_camera.flow(true, "Flow");
  blender::bNodeSocket &set_camera_input = set_camera.in_object("Camera");
  set_camera.in_int("User", 1);
  set_camera.flow(false, "Out");

  std::deque<blender::bNodeLink> links;
  InitLink(links.emplace_back(), on_update.node, tick, branch.node, branch_flow);
  InitLink(links.emplace_back(), player.node, player_value, view.node, view_user);
  InitLink(links.emplace_back(), view.node, shown, branch.node, condition);
  InitLink(links.emplace_back(), branch.node, branch_true, print_top.node, print_top_flow);
  InitLink(links.emplace_back(), view.node, top, print_top.node, print_top_value);
  InitLink(links.emplace_back(), branch.node, branch_true, print_views.node, print_views_flow);
  InitLink(links.emplace_back(), view.node, views, print_views.node, print_views_value);
  InitLink(links.emplace_back(), branch.node, branch_true, set_camera.node, set_flow);
  InitLink(links.emplace_back(), view.node, view_camera, set_camera.node, set_camera_input);
  for (blender::bNodeLink &link : links) {
    Append(tree.links, link);
  }

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(program, nullptr);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  std::vector<std::string> validation_errors;
  EXPECT_TRUE(program->ValidateInstructionPayloads(&validation_errors))
      << (validation_errors.empty() ? std::string() : validation_errors.front());
  /* The views are the scene's: the tree runs on the main thread. */
  EXPECT_EQ(program->GetSchedulerSummary().purity, LN_SchedulerPurity::MainThreadOnly);
  const std::vector<LN_Instruction> &instructions = program->GetInstructions(LN_Event::OnUpdate);

  const LN_Instruction *route = FindBranchRouteWithConditionKind(
      *program, instructions, LN_BoolExpressionKind::PlayerViewShown);
  ASSERT_NE(route, nullptr);
  const LN_BoolExpression *shown_expression = BoolExpressionAt(*program, route->bool_expr_index);
  ASSERT_NE(shown_expression, nullptr);
  const LN_IntExpression *user_expression = IntExpressionAt(*program, shown_expression->input0);
  ASSERT_NE(user_expression, nullptr);
  EXPECT_EQ(user_expression->kind, LN_IntExpressionKind::FromGenericValue);

  bool printed_top = false;
  bool printed_views = false;
  for (const LN_Instruction &instruction : instructions) {
    if (instruction.opcode != LN_OpCode::Print) {
      continue;
    }
    const LN_ValueExpression *printed = ValueExpressionAt(*program, instruction.value_expr_index);
    ASSERT_NE(printed, nullptr);
    if (printed->kind == LN_ValueExpressionKind::FromFloat) {
      const LN_FloatExpression *area = FloatExpressionAt(*program, printed->input0);
      ASSERT_NE(area, nullptr);
      EXPECT_EQ(area->kind, LN_FloatExpressionKind::PlayerViewArea);
      EXPECT_EQ(area->component_index, 1);
      printed_top = true;
    }
    else if (printed->kind == LN_ValueExpressionKind::FromInt) {
      const LN_IntExpression *count = IntExpressionAt(*program, printed->input0);
      ASSERT_NE(count, nullptr);
      EXPECT_EQ(count->kind, LN_IntExpressionKind::PlayerViewCount);
      printed_views = true;
    }
  }
  EXPECT_TRUE(printed_top);
  EXPECT_TRUE(printed_views);

  const LN_Instruction *set_camera_instruction = FindInstruction(instructions,
                                                                 LN_OpCode::SetActiveCamera);
  ASSERT_NE(set_camera_instruction, nullptr);
  const LN_ValueExpression *camera = ValueExpressionAt(*program,
                                                       set_camera_instruction->value_expr_index);
  ASSERT_NE(camera, nullptr);
  EXPECT_EQ(camera->kind, LN_ValueExpressionKind::PlayerViewCamera);
}

/* Split screen: hide an object from one player's camera, the camera and Visible read while the
 * game runs, and an empty Object (the tree's owner). */
TEST(LN_TreeCompiler, SetCameraVisibilityReadsItsInputsWhileTheGameRuns)
{
  blender::bNodeTree tree = {};
  InitTree(tree);
  TestNode on_update(tree, "LogicNativeOnUpdate", "On Update", 1);
  blender::bNodeSocket &tick = on_update.flow(false, "Out");
  TestNode player(tree, "LogicNativeGetGameProperty", "Get Object Property", 2);
  blender::bNodeSocket &player_value = AddPropertyRead(player, "player");
  TestNode view(tree, "LogicNativeGetPlayerView", "Get Player View", 3);
  blender::bNodeSocket &view_user = view.in_int("User", 0);
  blender::bNodeSocket &view_camera = view.add(false, "Camera", blender::SOCK_OBJECT, nullptr);
  view.add(false, "Shown", blender::SOCK_BOOLEAN, nullptr);
  view.add(false, "Left", blender::SOCK_FLOAT, nullptr);
  view.add(false, "Top", blender::SOCK_FLOAT, nullptr);
  view.add(false, "Width", blender::SOCK_FLOAT, nullptr);
  view.add(false, "Height", blender::SOCK_FLOAT, nullptr);
  view.add(false, "Views", blender::SOCK_INT, nullptr);
  TestNode shown(tree, "LogicNativeGetGameProperty", "Get Object Property.001", 4);
  blender::bNodeSocket &shown_value = AddPropertyRead(shown, "shown");
  TestNode hide(tree, "LogicNativeSetCameraVisibility", "Set Camera Visibility", 5);
  blender::bNodeSocket &hide_flow = hide.flow(true, "Flow");
  hide.in_object("Object");
  blender::bNodeSocket &camera_input = hide.in_object("Camera");
  blender::bNodeSocket &visible = hide.in_bool("Visible", false);
  hide.in_bool("Keep Shadow", true);
  hide.in_bool("Include Children", false);
  hide.flow(false, "Done");

  std::deque<blender::bNodeLink> links;
  InitLink(links.emplace_back(), on_update.node, tick, hide.node, hide_flow);
  InitLink(links.emplace_back(), player.node, player_value, view.node, view_user);
  InitLink(links.emplace_back(), view.node, view_camera, hide.node, camera_input);
  InitLink(links.emplace_back(), shown.node, shown_value, hide.node, visible);
  for (blender::bNodeLink &link : links) {
    Append(tree.links, link);
  }

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(program, nullptr);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  std::vector<std::string> validation_errors;
  EXPECT_TRUE(program->ValidateInstructionPayloads(&validation_errors))
      << (validation_errors.empty() ? std::string() : validation_errors.front());
  const LN_Instruction *instruction = FindInstruction(program->GetInstructions(LN_Event::OnUpdate),
                                                      LN_OpCode::SetCameraVisibility);
  ASSERT_NE(instruction, nullptr);
  const LN_ValueExpression *camera = ValueExpressionAt(*program, instruction->value_expr_index);
  ASSERT_NE(camera, nullptr);
  EXPECT_EQ(camera->kind, LN_ValueExpressionKind::PlayerViewCamera);
  /* No object: the rule is for the tree's owner. */
  EXPECT_EQ(instruction->secondary_value_expr_index, LN_INVALID_INDEX);
  const LN_BoolExpression *visible_expression = BoolExpressionAt(*program,
                                                                 instruction->bool_expr_index);
  ASSERT_NE(visible_expression, nullptr);
  EXPECT_NE(visible_expression->kind, LN_BoolExpressionKind::Constant);
  const LN_BoolExpression *keep_shadow = BoolExpressionAt(*program,
                                                          instruction->secondary_bool_expr_index);
  ASSERT_NE(keep_shadow, nullptr);
  EXPECT_TRUE(keep_shadow->bool_value);
  const LN_BoolExpression *include_children = BoolExpressionAt(
      *program, instruction->tertiary_bool_expr_index);
  ASSERT_NE(include_children, nullptr);
  EXPECT_FALSE(include_children->bool_value);
}

/* The computer: Take UI Control for the player read while the game runs, then (on Done) that
 * player's Set Player Pointer at a speed read while the game runs. */
TEST(LN_TreeCompiler, TakeUIControlAndSetPlayerPointerReadTheirInputsWhileTheGameRuns)
{
  blender::bNodeTree tree = {};
  InitTree(tree);
  TestNode on_update(tree, "LogicNativeOnUpdate", "On Update", 1);
  blender::bNodeSocket &tick = on_update.flow(false, "Out");
  TestNode player(tree, "LogicNativeGetGameProperty", "Get Object Property", 2);
  blender::bNodeSocket &player_value = AddPropertyRead(player, "player");
  TestNode speed(tree, "LogicNativeGetGameProperty", "Get Object Property.001", 3);
  blender::bNodeSocket &speed_value = AddPropertyRead(speed, "speed");
  TestNode take(tree, "LogicNativeTakeUIControl", "Take UI Control", 4);
  blender::bNodeSocket &take_flow = take.flow(true, "Flow");
  take.in_object("Object");
  blender::bNodeSocket &take_user = take.in_int("User", 0);
  take.in_bool("Take", true);
  blender::bNodeSocket &done = take.flow(false, "Done");
  take.flow(false, "Failed");
  take.add(false, "Result", blender::SOCK_INT, nullptr);
  TestNode me(tree, "LogicNativeGetOwner", "Get Owner", 6);
  blender::bNodeSocket &me_out = me.add(false, "Owner Object", blender::SOCK_OBJECT, nullptr);
  TestNode pointer(tree, "LogicNativeSetPlayerPointer", "Set Player Pointer", 5);
  blender::bNodeSocket &pointer_flow = pointer.flow(true, "Flow");
  blender::bNodeSocket &pointer_user = pointer.in_int("User", 0);
  pointer.in_bool("Enabled", true);
  blender::bNodeSocket &surface = pointer.in_object("Surface");
  pointer.in_object("Cursor");
  pointer.in_string("Move Action", "Move");
  blender::bNodeSocket &pointer_speed = pointer.in_float("Speed", 1.0f);
  pointer.in_float("Reach", 3.0f);
  blender::bNodeSocket &press_action = pointer.in_string("Press Action", "");
  pointer.flow(false, "Done");

  std::deque<blender::bNodeLink> links;
  InitLink(links.emplace_back(), on_update.node, tick, take.node, take_flow);
  InitLink(links.emplace_back(), player.node, player_value, take.node, take_user);
  InitLink(links.emplace_back(), take.node, done, pointer.node, pointer_flow);
  InitLink(links.emplace_back(), player.node, player_value, pointer.node, pointer_user);
  InitLink(links.emplace_back(), speed.node, speed_value, pointer.node, pointer_speed);
  InitLink(links.emplace_back(), me.node, me_out, pointer.node, surface);
  for (blender::bNodeLink &link : links) {
    Append(tree.links, link);
  }

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(program, nullptr);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  std::vector<std::string> validation_errors;
  EXPECT_TRUE(program->ValidateInstructionPayloads(&validation_errors))
      << (validation_errors.empty() ? std::string() : validation_errors.front());
  const std::vector<LN_Instruction> &instructions = program->GetInstructions(LN_Event::OnUpdate);

  const LN_Instruction *take_instruction = FindInstruction(instructions,
                                                           LN_OpCode::TakeUIControl);
  ASSERT_NE(take_instruction, nullptr);
  /* No Object: the tree's own. */
  EXPECT_EQ(take_instruction->value_expr_index, LN_INVALID_INDEX);
  const LN_IntExpression *take_player = IntExpressionAt(*program,
                                                        take_instruction->int_expr_index);
  ASSERT_NE(take_player, nullptr);
  EXPECT_EQ(take_player->kind, LN_IntExpressionKind::FromGenericValue);
  const LN_BoolExpression *completion = BoolExpressionAt(*program,
                                                         take_instruction->property_ref_index);
  ASSERT_NE(completion, nullptr);
  EXPECT_EQ(completion->kind, LN_BoolExpressionKind::UICommandCompleted);

  const LN_BoolExpression *take_value = BoolExpressionAt(*program,
                                                         take_instruction->bool_expr_index);
  ASSERT_NE(take_value, nullptr);
  EXPECT_TRUE(take_value->bool_value);

  const LN_Instruction *pointer_instruction = FindInstruction(instructions,
                                                              LN_OpCode::SetPlayerPointer);
  ASSERT_NE(pointer_instruction, nullptr);
  /* It runs on Take UI Control's Done: once the engine gave the player the computer. */
  const LN_BoolExpression *guard = BoolExpressionAt(*program,
                                                    pointer_instruction->bool_guard_expr_index);
  ASSERT_NE(guard, nullptr);
  EXPECT_EQ(guard->kind, LN_BoolExpressionKind::UICommandSucceeded);
  EXPECT_EQ(guard->input0, take_instruction->property_ref_index);
  /* The linked Surface is read while the game runs; no Cursor is none. */
  EXPECT_NE(pointer_instruction->value_expr_index, LN_INVALID_INDEX);
  EXPECT_EQ(pointer_instruction->secondary_value_expr_index, LN_INVALID_INDEX);
  const LN_StringExpression *move_action = StringExpressionAt(
      *program, pointer_instruction->string_expr_index);
  ASSERT_NE(move_action, nullptr);
  EXPECT_EQ(move_action->string_value, "Move");
  const LN_IntExpression *pointer_player = IntExpressionAt(*program,
                                                           pointer_instruction->int_expr_index);
  ASSERT_NE(pointer_player, nullptr);
  EXPECT_EQ(pointer_player->kind, LN_IntExpressionKind::FromGenericValue);
  const LN_FloatExpression *speed_expression = FloatExpressionAt(
      *program, pointer_instruction->float_expr_index);
  ASSERT_NE(speed_expression, nullptr);
  EXPECT_NE(speed_expression->kind, LN_FloatExpressionKind::Constant);
  const LN_BoolExpression *enabled = BoolExpressionAt(*program,
                                                      pointer_instruction->bool_expr_index);
  ASSERT_NE(enabled, nullptr);
  EXPECT_TRUE(enabled->bool_value);
  /* No Press Action: Accept. */
  const LN_StringExpression *press = StringExpressionAt(
      *program, pointer_instruction->secondary_string_expr_index);
  ASSERT_NE(press, nullptr);
  EXPECT_EQ(press->string_value, "");
  EXPECT_FALSE(pointer_instruction->bool_value);
  (void)press_action;
}

/* Gamepad Look reads its stick as it is (no axis flipped unless Inverted says so) and keeps the
 * gamepad's Index, linked here, for the player whose menu may use it. */
TEST(LN_TreeCompiler, GamepadLookReadsItsStickUnflippedAndKeepsItsGamepad)
{
  blender::bNodeTree tree = {};
  InitTree(tree);
  TestNode step(tree, "LogicNativeOnFixedUpdate", "On Logic + Physics Step", 1);
  blender::bNodeSocket &tick = step.flow(false, "Out");
  TestNode pad(tree, "LogicNativeGetGameProperty", "Get Object Property", 2);
  blender::bNodeSocket &pad_value = AddPropertyRead(pad, "pad");
  TestNode look(tree, "LogicNativeGamepadLook", "Gamepad Look", 3);
  look.node.custom1 = 1;
  blender::bNodeSocket &look_flow = look.flow(true, "Flow");
  look.in_object("Body Object");
  look.in_object("Head Object");
  look.in_vector("Inverted", MT_Vector3(0.0f, 0.0f, 0.0f));
  blender::bNodeSocket &index = look.in_int("Index", 0);
  look.in_float("Sensitivity", 3.14f);
  look.in_float("Exponent", 2.3f);
  look.in_bool("Cap Left / Right", false);
  look.in_vector("Left / Right Range", MT_Vector3(0.0f, 0.0f, 0.0f));
  look.in_bool("Cap Up / Down", true);
  look.in_vector("Up / Down Range", MT_Vector3(-1.4f, 1.4f, 0.0f));
  look.in_float("Threshold", 0.1f);
  look.flow(false, "Done");

  std::deque<blender::bNodeLink> links;
  InitLink(links.emplace_back(), step.node, tick, look.node, look_flow);
  InitLink(links.emplace_back(), pad.node, pad_value, look.node, index);
  for (blender::bNodeLink &link : links) {
    Append(tree.links, link);
  }

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(program, nullptr);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  const LN_Instruction *instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::GamepadLook);
  ASSERT_NE(instruction, nullptr);
  const LN_VectorExpression *stick = VectorExpressionAt(*program, instruction->vector_expr_index);
  ASSERT_NE(stick, nullptr);
  EXPECT_EQ(stick->kind, LN_VectorExpressionKind::GamepadStick);
  EXPECT_EQ(stick->property_ref_index, 0u);
  EXPECT_EQ(stick->input2, 1u);
  EXPECT_EQ(instruction->secondary_int_expr_index, stick->input0);
  const LN_IntExpression *gamepad = IntExpressionAt(*program, instruction->secondary_int_expr_index);
  ASSERT_NE(gamepad, nullptr);
  EXPECT_NE(gamepad->kind, LN_IntExpressionKind::Constant);
}

/* Set Player Pointer's View Center reads Reach (linked) and Press Action while the game runs, and
 * not the Surface inputs it hides. Set Cursor Visibility's Only in Menus is the instruction's
 * mode. */
TEST(LN_TreeCompiler, ViewCenterPointerReadsReachAndPressActionWhileTheGameRuns)
{
  blender::bNodeTree tree = {};
  InitTree(tree);
  TestNode on_update(tree, "LogicNativeOnUpdate", "On Update", 1);
  blender::bNodeSocket &tick = on_update.flow(false, "Out");
  TestNode reach(tree, "LogicNativeGetGameProperty", "Get Object Property", 2);
  blender::bNodeSocket &reach_value = AddPropertyRead(reach, "reach");
  TestNode pointer(tree, "LogicNativeSetPlayerPointer", "Set Player Pointer", 3);
  pointer.node.custom1 = 1;
  blender::bNodeSocket &pointer_flow = pointer.flow(true, "Flow");
  pointer.in_int("User", 1);
  pointer.in_bool("Enabled", true);
  pointer.in_object("Surface");
  pointer.in_object("Cursor");
  pointer.in_string("Move Action", "Move");
  pointer.in_float("Speed", 1.0f);
  blender::bNodeSocket &pointer_reach = pointer.in_float("Reach", 3.0f);
  pointer.in_string("Press Action", "Use");
  blender::bNodeSocket &pointer_done = pointer.flow(false, "Done");
  TestNode cursor(tree, "LogicNativeSetCursorVisibility", "Set Cursor Visibility", 4);
  cursor.node.custom1 = 1;
  blender::bNodeSocket &cursor_flow = cursor.flow(true, "Flow");
  cursor.in_bool("Visible", false);
  cursor.flow(false, "Done");

  std::deque<blender::bNodeLink> links;
  InitLink(links.emplace_back(), on_update.node, tick, pointer.node, pointer_flow);
  InitLink(links.emplace_back(), reach.node, reach_value, pointer.node, pointer_reach);
  InitLink(links.emplace_back(), pointer.node, pointer_done, cursor.node, cursor_flow);
  for (blender::bNodeLink &link : links) {
    Append(tree.links, link);
  }

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(program, nullptr);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  std::vector<std::string> validation_errors;
  EXPECT_TRUE(program->ValidateInstructionPayloads(&validation_errors))
      << (validation_errors.empty() ? std::string() : validation_errors.front());
  const std::vector<LN_Instruction> &instructions = program->GetInstructions(LN_Event::OnUpdate);
  const LN_Instruction *pointer_instruction = FindInstruction(instructions,
                                                              LN_OpCode::SetPlayerPointer);
  ASSERT_NE(pointer_instruction, nullptr);
  EXPECT_TRUE(pointer_instruction->bool_value);
  /* View Center has no Surface, Cursor, Move Action or Speed. */
  EXPECT_EQ(pointer_instruction->value_expr_index, LN_INVALID_INDEX);
  EXPECT_EQ(pointer_instruction->secondary_value_expr_index, LN_INVALID_INDEX);
  EXPECT_EQ(pointer_instruction->string_expr_index, LN_INVALID_INDEX);
  EXPECT_EQ(pointer_instruction->float_expr_index, LN_INVALID_INDEX);
  const LN_FloatExpression *reach_expression = FloatExpressionAt(
      *program, pointer_instruction->secondary_float_expr_index);
  ASSERT_NE(reach_expression, nullptr);
  EXPECT_NE(reach_expression->kind, LN_FloatExpressionKind::Constant);
  const LN_StringExpression *press = StringExpressionAt(
      *program, pointer_instruction->secondary_string_expr_index);
  ASSERT_NE(press, nullptr);
  EXPECT_EQ(press->string_value, "Use");

  const LN_Instruction *cursor_instruction = FindInstruction(instructions,
                                                             LN_OpCode::SetCursorVisibility);
  ASSERT_NE(cursor_instruction, nullptr);
  EXPECT_EQ(cursor_instruction->int_value, 1);
}

/* Split screen: a HUD in one player's view, the player read while the game runs. */
TEST(LN_TreeCompiler, ScreenAnchorInAPlayersViewReadsTheUserWhileTheGameRuns)
{
  blender::bNodeTree tree = {};
  InitTree(tree);
  TestNode on_update(tree, "LogicNativeOnUpdate", "On Update", 1);
  blender::bNodeSocket &tick = on_update.flow(false, "Out");
  TestNode player(tree, "LogicNativeGetGameProperty", "Get Object Property", 2);
  blender::bNodeSocket &player_value = AddPropertyRead(player, "player");
  TestNode anchor(tree, "LogicNativeScreenAnchor", "Screen Anchor", 3);
  anchor.node.custom1 = 0;
  anchor.node.custom2 = 1;
  blender::bNodeSocket &anchor_flow = anchor.flow(true, "Flow");
  anchor.in_object("Object");
  anchor.in_object("Camera");
  blender::bNodeSocket &user = anchor.in_int("User", 0);
  std::array<blender::bNodeSocketValueVector, 3> vectors = {};
  vectors[2].value[0] = 1920.0f;
  vectors[2].value[1] = 1080.0f;
  anchor.add(true, "Offset Pixels", blender::SOCK_VECTOR, &vectors[0]);
  anchor.add(true, "Safe Margin Pixels", blender::SOCK_VECTOR, &vectors[1]);
  anchor.add(true, "Reference Resolution", blender::SOCK_VECTOR, &vectors[2]);
  anchor.in_float("Depth", 1.0f);
  anchor.in_bool("Scale Offset", true);
  anchor.flow(false, "Done");
  std::deque<blender::bNodeLink> links;
  InitLink(links.emplace_back(), on_update.node, tick, anchor.node, anchor_flow);
  InitLink(links.emplace_back(), player.node, player_value, anchor.node, user);
  for (blender::bNodeLink &link : links) {
    Append(tree.links, link);
  }
  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(program, nullptr);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  std::vector<std::string> validation_errors;
  EXPECT_TRUE(program->ValidateInstructionPayloads(&validation_errors))
      << (validation_errors.empty() ? std::string() : validation_errors.front());
  const LN_Instruction *instruction = FindInstruction(program->GetInstructions(LN_Event::OnUpdate),
                                                      LN_OpCode::ScreenAnchor);
  ASSERT_NE(instruction, nullptr);
  EXPECT_EQ(instruction->secondary_int_value, 1);
  const LN_IntExpression *user_expression = IntExpressionAt(*program,
                                                            instruction->int_expr_index);
  ASSERT_NE(user_expression, nullptr);
  EXPECT_EQ(user_expression->kind, LN_IntExpressionKind::FromGenericValue);
}

/* Per-player menus: Open Object Menu's Area and Owner (read while the game runs), and Get Menu
 * Player's User and Owner reaching their readers. */
TEST(LN_TreeCompiler, PerPlayerMenuNodesReachTheirReaders)
{
  blender::bNodeTree tree = {};
  InitTree(tree);
  TestNode pressed(tree, "LogicNativeInputAction", "Input Action", 1);
  pressed.in_string("Action", "Inventory");
  pressed.in_int("User", 0);
  blender::bNodeSocket &out = pressed.flow(false, "Out");
  TestNode me(tree, "LogicNativeGetOwner", "Get Owner", 2);
  blender::bNodeSocket &me_out = me.add(false, "Owner Object", blender::SOCK_OBJECT, nullptr);
  TestNode player(tree, "LogicNativeGetGameProperty", "Get Object Property", 3);
  blender::bNodeSocket &player_value = AddPropertyRead(player, "user");
  TestNode open(tree, "LogicNativeOpenObjectMenu", "Open Object Menu", 4);
  open.node.custom2 = int16_t(LN_PackUIMenuNodeSettings(LN_UIMenuOpenMode::Push,
                                                        LN_UIMenuPresentation::Screen,
                                                        LN_UIMenuBackBehavior::CloseMenu) |
                              LN_UI_MENU_NODE_PLAYER_VIEW);
  blender::bNodeSocket &open_flow = open.flow(true, "Flow");
  open.in_object("Camera");
  blender::bNodeSocketValueCollection no_collection = {};
  open.add(true, "Collection", blender::SOCK_COLLECTION, &no_collection);
  open.in_string("Input Context", "");
  blender::bNodeSocket &open_user = open.in_int("User", 0);
  blender::bNodeSocket &open_owner = open.in_object("Owner");
  open.flow(false, "Opened");
  open.flow(false, "Failed");
  open.flow(false, "Closed");
  open.add(false, "Menu", blender::SOCK_INT, nullptr);
  open.add(false, "Result", blender::SOCK_INT, nullptr);

  TestNode on_update(tree, "LogicNativeOnUpdate", "On Update", 5);
  blender::bNodeSocket &tick = on_update.flow(false, "Out");
  TestNode menu(tree, "LogicNativeGetMenuPlayer", "Get Menu Player", 6);
  blender::bNodeSocket &menu_user = menu.add(false, "User", blender::SOCK_INT, nullptr);
  blender::bNodeSocket &menu_owner = menu.add(false, "Owner", blender::SOCK_OBJECT, nullptr);
  TestNode gold(tree, "LogicNativeGetGamePropertyInt", "Get Game Property Int", 7);
  blender::bNodeSocket &gold_object = gold.in_object("Object");
  gold.in_string("Property", "gold");
  blender::bNodeSocket &gold_value = gold.add(false, "Value", blender::SOCK_INT, nullptr);
  TestNode print_user(tree, "LogicNativePrint", "Print", 8);
  blender::bNodeSocket &print_user_flow = print_user.flow(true, "Flow");
  blender::bNodeSocket &print_user_value = print_user.in_string("Value", "");
  TestNode print_gold(tree, "LogicNativePrint", "Print.001", 9);
  blender::bNodeSocket &print_gold_flow = print_gold.flow(true, "Flow");
  blender::bNodeSocket &print_gold_value = print_gold.in_string("Value", "");

  std::deque<blender::bNodeLink> links;
  InitLink(links.emplace_back(), pressed.node, out, open.node, open_flow);
  InitLink(links.emplace_back(), player.node, player_value, open.node, open_user);
  InitLink(links.emplace_back(), me.node, me_out, open.node, open_owner);
  InitLink(links.emplace_back(), on_update.node, tick, print_user.node, print_user_flow);
  InitLink(links.emplace_back(), menu.node, menu_user, print_user.node, print_user_value);
  InitLink(links.emplace_back(), on_update.node, tick, print_gold.node, print_gold_flow);
  InitLink(links.emplace_back(), menu.node, menu_owner, gold.node, gold_object);
  InitLink(links.emplace_back(), gold.node, gold_value, print_gold.node, print_gold_value);
  for (blender::bNodeLink &link : links) {
    Append(tree.links, link);
  }

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(program, nullptr);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  std::vector<std::string> validation_errors;
  EXPECT_TRUE(program->ValidateInstructionPayloads(&validation_errors))
      << (validation_errors.empty() ? std::string() : validation_errors.front());

  const LN_Instruction *open_instruction = nullptr;
  for (const LN_Event event : {LN_Event::OnUpdate, LN_Event::OnFixedUpdate}) {
    if (open_instruction == nullptr) {
      open_instruction = FindInstruction(program->GetInstructions(event),
                                         LN_OpCode::OpenObjectMenu);
    }
  }
  ASSERT_NE(open_instruction, nullptr);
  EXPECT_TRUE(LN_UnpackUIMenuInPlayerView(open_instruction->secondary_int_value));
  const LN_ValueExpression *owner = ValueExpressionAt(*program,
                                                      open_instruction->tertiary_value_expr_index);
  ASSERT_NE(owner, nullptr);
  EXPECT_NE(owner->kind, LN_ValueExpressionKind::Constant);

  const auto &ints = program->GetIntExpressions();
  EXPECT_TRUE(std::any_of(ints.begin(), ints.end(), [](const LN_IntExpression &expression) {
    return expression.kind == LN_IntExpressionKind::MenuPlayerUser;
  }));
  const auto &values = program->GetValueExpressions();
  EXPECT_TRUE(std::any_of(values.begin(), values.end(), [](const LN_ValueExpression &expression) {
    return expression.kind == LN_ValueExpressionKind::MenuPlayerOwner;
  }));
  EXPECT_EQ(program->GetSchedulerSummary().purity, LN_SchedulerPurity::MainThreadOnly);
}

/* Joining: On Player Join's player and gamepad reach their readers; Get Player Gamepad and Set
 * Player Gamepad read their player while the game runs. */
TEST(LN_TreeCompiler, PlayerJoinAndPlayerGamepadNodesReachTheirReaders)
{
  blender::bNodeTree tree = {};
  InitTree(tree);
  TestNode join(tree, "LogicNativeOnPlayerJoin", "On Player Join", 1);
  blender::bNodeSocket &joined = join.flow(false, "Out");
  blender::bNodeSocket &join_user = join.add(false, "User", blender::SOCK_INT, nullptr);
  join.add(false, "Gamepad", blender::SOCK_INT, nullptr);
  TestNode print_user(tree, "LogicNativePrint", "Print", 2);
  blender::bNodeSocket &print_user_flow = print_user.flow(true, "Flow");
  blender::bNodeSocket &print_user_value = print_user.in_string("Value", "");
  TestNode give(tree, "LogicNativeSetPlayerGamepad", "Set Player Gamepad", 3);
  blender::bNodeSocket &give_flow = give.flow(true, "Flow");
  blender::bNodeSocket &give_user = give.in_int("User", 0);
  give.in_int("Gamepad", -1);
  give.flow(false, "Out");
  TestNode on_update(tree, "LogicNativeOnUpdate", "On Update", 4);
  blender::bNodeSocket &tick = on_update.flow(false, "Out");
  TestNode player(tree, "LogicNativeGetGameProperty", "Get Object Property", 5);
  blender::bNodeSocket &player_value = AddPropertyRead(player, "player");
  TestNode pad(tree, "LogicNativeGetPlayerGamepad", "Get Player Gamepad", 6);
  blender::bNodeSocket &pad_user = pad.in_int("User", 0);
  blender::bNodeSocket &pad_number = pad.add(false, "Gamepad", blender::SOCK_INT, nullptr);
  blender::bNodeSocket &connected = pad.add(false, "Connected", blender::SOCK_BOOLEAN, nullptr);
  pad.add(false, "Joined", blender::SOCK_BOOLEAN, nullptr);
  TestNode branch(tree, "LogicNativeBranch", "Branch", 7);
  blender::bNodeSocket &branch_flow = branch.flow(true, "Flow");
  blender::bNodeSocket &condition = branch.in_bool("Condition", false);
  blender::bNodeSocket &branch_true = branch.flow(false, "True");
  branch.flow(false, "False");
  TestNode print_pad(tree, "LogicNativePrint", "Print.001", 8);
  blender::bNodeSocket &print_pad_flow = print_pad.flow(true, "Flow");
  blender::bNodeSocket &print_pad_value = print_pad.in_string("Value", "");

  std::deque<blender::bNodeLink> links;
  InitLink(links.emplace_back(), join.node, joined, print_user.node, print_user_flow);
  InitLink(links.emplace_back(), join.node, join_user, print_user.node, print_user_value);
  InitLink(links.emplace_back(), join.node, joined, give.node, give_flow);
  InitLink(links.emplace_back(), join.node, join_user, give.node, give_user);
  InitLink(links.emplace_back(), on_update.node, tick, branch.node, branch_flow);
  InitLink(links.emplace_back(), player.node, player_value, pad.node, pad_user);
  InitLink(links.emplace_back(), pad.node, connected, branch.node, condition);
  InitLink(links.emplace_back(), branch.node, branch_true, print_pad.node, print_pad_flow);
  InitLink(links.emplace_back(), pad.node, pad_number, print_pad.node, print_pad_value);
  for (blender::bNodeLink &link : links) {
    Append(tree.links, link);
  }

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  ASSERT_NE(program, nullptr);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  std::vector<std::string> validation_errors;
  EXPECT_TRUE(program->ValidateInstructionPayloads(&validation_errors))
      << (validation_errors.empty() ? std::string() : validation_errors.front());
  EXPECT_EQ(program->GetSchedulerSummary().purity, LN_SchedulerPurity::MainThreadOnly);
  const std::vector<LN_Instruction> &instructions = program->GetInstructions(LN_Event::OnUpdate);

  /* On Player Join runs its route in the frame update, like On Game Loaded. */
  EXPECT_NE(FindBranchRouteWithConditionKind(
                *program, instructions, LN_BoolExpressionKind::PlayerJoinPulse),
            nullptr);
  const LN_Instruction *give_instruction = FindInstruction(instructions,
                                                           LN_OpCode::SetPlayerGamepad);
  ASSERT_NE(give_instruction, nullptr);
  const LN_IntExpression *given_user = IntExpressionAt(*program,
                                                       give_instruction->int_expr_index);
  ASSERT_NE(given_user, nullptr);
  EXPECT_EQ(given_user->kind, LN_IntExpressionKind::PlayerJoinUser);
  const LN_IntExpression *given_pad = IntExpressionAt(
      *program, give_instruction->secondary_int_expr_index);
  ASSERT_NE(given_pad, nullptr);
  EXPECT_EQ(given_pad->int_value, -1);

  EXPECT_NE(FindBranchRouteWithConditionKind(
                *program, instructions, LN_BoolExpressionKind::PlayerGamepadConnected),
            nullptr);
  bool printed_user = false;
  bool printed_pad = false;
  for (const LN_Instruction &instruction : instructions) {
    if (instruction.opcode != LN_OpCode::Print) {
      continue;
    }
    const LN_ValueExpression *printed = ValueExpressionAt(*program, instruction.value_expr_index);
    ASSERT_NE(printed, nullptr);
    ASSERT_EQ(printed->kind, LN_ValueExpressionKind::FromInt);
    const LN_IntExpression *value = IntExpressionAt(*program, printed->input0);
    ASSERT_NE(value, nullptr);
    if (value->kind == LN_IntExpressionKind::PlayerJoinUser) {
      printed_user = true;
    }
    if (value->kind == LN_IntExpressionKind::PlayerGamepad) {
      const LN_IntExpression *user = IntExpressionAt(*program, value->input0);
      ASSERT_NE(user, nullptr);
      EXPECT_EQ(user->kind, LN_IntExpressionKind::FromGenericValue);
      printed_pad = true;
    }
  }
  EXPECT_TRUE(printed_user);
  EXPECT_TRUE(printed_pad);
}

/* The program's only Collection Objects expression (Get Collection Objects or a collection
 * linked into a list input). */
const LN_ValueExpression *OnlyCollectionObjects(const LN_Program &program)
{
  const LN_ValueExpression *found = nullptr;
  for (const LN_ValueExpression &expression : program.GetValueExpressions()) {
    if (expression.kind == LN_ValueExpressionKind::CollectionObjects) {
      if (found != nullptr) {
        return nullptr;
      }
      found = &expression;
    }
  }
  return found;
}

/* Set Collection Member reads its Member input while the game runs; an empty Object is the
 * tree's own object, and a missing collection is an error. */
TEST(LN_TreeCompiler, SetCollectionMemberCompilesRuntimeInputs)
{
  blender::Collection collection = {};
  SetCString(collection.id.name, "GRMinimap");
  for (const bool with_collection : {true, false}) {
    blender::bNodeSocketValueCollection collection_default = {};
    collection_default.value = with_collection ? &collection : nullptr;
    blender::bNodeTree tree = {};
    InitTree(tree);
    TestNode on_update(tree, "LogicNativeOnUpdate", "On Update", 1);
    blender::bNodeSocket &tick = on_update.flow(false, "Out");
    TestNode shown(tree, "LogicNativeGetGamePropertyBool", "Get Shown", 2);
    shown.in_object("Object");
    shown.in_string("Property", "shown");
    blender::bNodeSocket &shown_value = shown.add(false, "Value", blender::SOCK_BOOLEAN, nullptr);
    TestNode set_member(tree, "LogicNativeSetCollectionMember", "Set Collection Member", 3);
    blender::bNodeSocket &flow = set_member.flow(true, "Flow");
    set_member.in_object("Object");
    set_member.add(true, "Collection", blender::SOCK_COLLECTION, &collection_default);
    blender::bNodeSocket &member = set_member.in_bool("Member", true);
    set_member.in_bool("Include Children", true);
    set_member.flow(false, "Done");
    std::array<blender::bNodeLink, 2> links = {};
    InitLink(links[0], on_update.node, tick, set_member.node, flow);
    InitLink(links[1], shown.node, shown_value, set_member.node, member);
    for (blender::bNodeLink &link : links) {
      Append(tree.links, link);
    }

    const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
    ASSERT_NE(program, nullptr);
    const LN_Instruction *instruction = FindInstruction(
        program->GetInstructions(LN_Event::OnUpdate), LN_OpCode::SetCollectionMember);
    if (!with_collection) {
      EXPECT_TRUE(program->GetCompileReport().HasErrors());
      EXPECT_EQ(instruction, nullptr);
      EXPECT_NE(FormatCompileReport(program->GetCompileReport()).find("requires a collection"),
                std::string::npos);
      continue;
    }
    EXPECT_FALSE(program->GetCompileReport().HasErrors())
        << FormatCompileReport(program->GetCompileReport());
    std::vector<std::string> validation_errors;
    EXPECT_TRUE(program->ValidateInstructionPayloads(&validation_errors))
        << (validation_errors.empty() ? std::string() : validation_errors.front());
    ASSERT_NE(instruction, nullptr);
    const LN_ValueExpression *target = ValueExpressionAt(*program,
                                                         instruction->value_expr_index);
    ASSERT_NE(target, nullptr);
    EXPECT_EQ(target->value.type, LN_ValueType::CollectionRef);
    EXPECT_EQ(instruction->secondary_value_expr_index, LN_INVALID_INDEX);
    const LN_BoolExpression *member_read = BoolExpressionAt(*program,
                                                            instruction->bool_expr_index);
    ASSERT_NE(member_read, nullptr);
    EXPECT_EQ(member_read->kind, LN_BoolExpressionKind::SnapshotGameProperty);
    const LN_BoolExpression *children = BoolExpressionAt(*program,
                                                         instruction->secondary_bool_expr_index);
    ASSERT_NE(children, nullptr);
    EXPECT_EQ(children->kind, LN_BoolExpressionKind::Constant);
    EXPECT_TRUE(children->bool_value);
  }
}

/* Get Collection Objects reads Include Child Collections while the game runs, and a collection
 * linked into a list input is the list of its objects, child collections included (the game
 * refused that link before 2026-10-15 although the editor allowed it). */
TEST(LN_TreeCompiler, CollectionObjectListsFollowRuntimeMembership)
{
  blender::Collection collection = {};
  SetCString(collection.id.name, "GRMinimap");
  blender::bNodeSocketValueCollection collection_default = {};
  collection_default.value = &collection;
  for (const bool through_list_input : {false, true}) {
    blender::bNodeTree tree = {};
    InitTree(tree);
    TestNode step(tree, "LogicNativeOnFixedUpdate", "On Logic + Physics Step", 1);
    blender::bNodeSocket &tick = step.flow(false, "Out");
    TestNode children(tree, "LogicNativeGetGamePropertyBool", "Get Children", 2);
    children.in_object("Object");
    children.in_string("Property", "children");
    blender::bNodeSocket &children_value = children.add(
        false, "Value", blender::SOCK_BOOLEAN, nullptr);
    TestNode get_collection(tree, "LogicNativeGetCollection", "Get Collection", 3);
    get_collection.add(true, "Collection", blender::SOCK_COLLECTION, &collection_default);
    blender::bNodeSocket &collection_out = get_collection.add(
        false, "Collection", blender::SOCK_COLLECTION, nullptr);
    TestNode objects(tree, "LogicNativeGetCollectionObjects", "Get Objects", 4);
    blender::bNodeSocket &objects_collection = objects.add(
        true, "Collection", blender::SOCK_COLLECTION, nullptr);
    blender::bNodeSocket &objects_children = objects.in_bool("Include Child Collections", false);
    blender::bNodeSocket &objects_out = objects.add(
        false, "Objects", blender::SOCK_CUSTOM, nullptr, "NodeSocketLogicList");
    TestNode length(tree, "LogicNativeListLength", "List Length", 5);
    blender::bNodeSocket &length_list = length.add(
        true, "List", blender::SOCK_CUSTOM, nullptr, "NodeSocketLogicList");
    blender::bNodeSocket &length_out = length.add(false, "Length", blender::SOCK_INT, nullptr);
    TestNode print(tree, "LogicNativePrint", "Print", 6);
    blender::bNodeSocket &print_flow = print.flow(true, "Flow");
    blender::bNodeSocket &print_value = print.in_string("Value", "");
    std::deque<blender::bNodeLink> links;
    InitLink(links.emplace_back(), step.node, tick, print.node, print_flow);
    InitLink(links.emplace_back(), length.node, length_out, print.node, print_value);
    if (through_list_input) {
      InitLink(links.emplace_back(),
               get_collection.node,
               collection_out,
               length.node,
               length_list);
    }
    else {
      InitLink(links.emplace_back(),
               get_collection.node,
               collection_out,
               objects.node,
               objects_collection);
      InitLink(links.emplace_back(), children.node, children_value, objects.node, objects_children);
      InitLink(links.emplace_back(), objects.node, objects_out, length.node, length_list);
    }
    for (blender::bNodeLink &link : links) {
      Append(tree.links, link);
    }

    const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
    ASSERT_NE(program, nullptr);
    EXPECT_FALSE(program->GetCompileReport().HasErrors())
        << FormatCompileReport(program->GetCompileReport());
    const LN_ValueExpression *list = OnlyCollectionObjects(*program);
    ASSERT_NE(list, nullptr);
    const LN_ValueExpression *source = ValueExpressionAt(*program, list->input0);
    ASSERT_NE(source, nullptr);
    EXPECT_EQ(source->value.type, LN_ValueType::CollectionRef);
    const LN_BoolExpression *include_children = BoolExpressionAt(*program,
                                                                 list->bool_expr_index);
    ASSERT_NE(include_children, nullptr);
    if (through_list_input) {
      EXPECT_EQ(include_children->kind, LN_BoolExpressionKind::Constant);
      EXPECT_TRUE(include_children->bool_value);
    }
    else {
      EXPECT_EQ(include_children->kind, LN_BoolExpressionKind::SnapshotGameProperty);
    }
    /* Without a scene the list is empty, and the tree still runs. */
    EXPECT_EQ(PrintsOfFixedSteps(program, 1).back(), std::vector<std::string>{"0"});
  }
}

}  // namespace
