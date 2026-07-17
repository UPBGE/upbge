/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "DNA_ID_enums.h"
#include "DNA_logic_node_binding_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_rigidbody_types.h"

#include "SCA_IInputDevice.h"
#include "SCA_InputEvent.h"

#include "LN_BindingSource.h"
#include "LN_TreeCompiler.h"

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
  EXPECT_EQ(program->GetInstructions(LN_Event::OnFixedUpdate).size(), 2);
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
  InitNode(set_position, "LogicNativeSetWorldPosition", "Set Position", 4);
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(position, "Position", blender::SOCK_VECTOR, &position_default);
  Append(set_position.inputs, flow);
  Append(set_position.inputs, position);
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
  EXPECT_EQ(
      FindInstruction(program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::BranchRoute),
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
  InitNode(set_position, "LogicNativeSetWorldPosition", "Set Position", 3);
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(position, "Position", blender::SOCK_VECTOR, &position_default);
  Append(set_position.inputs, flow);
  Append(set_position.inputs, position);
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

TEST(LN_TreeCompiler, RejectsStaleConditionExecutionSocket)
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
  InitNode(set_position, "LogicNativeSetWorldPosition", "Set Position", 2);
  InitSocket(stale_flow, "Condition", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(position, "Position", blender::SOCK_VECTOR, &position_default);
  Append(set_position.inputs, stale_flow);
  Append(set_position.inputs, position);
  Append(tree.nodes, set_position);

  blender::bNodeLink event_to_set_position = {};
  InitLink(event_to_set_position, on_update, on_update_pulse, set_position, stale_flow);
  Append(tree.links, event_to_set_position);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  const LN_CompileReport &report = program->GetCompileReport();

  EXPECT_TRUE(report.HasErrors()) << FormatCompileReport(report);
  EXPECT_TRUE(HasErrorContaining(report, "not part of the native node definition"))
      << FormatCompileReport(report);
}

TEST(LN_TreeCompiler, RejectsFlowSocketWithConditionSocketIdname)
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
  InitNode(set_position, "LogicNativeSetWorldPosition", "Set Position", 2);
  InitSocket(stale_flow, "Flow", blender::SOCK_BOOLEAN, nullptr, "NodeSocketLogicCondition");
  InitSocket(position, "Position", blender::SOCK_VECTOR, &position_default);
  Append(set_position.inputs, stale_flow);
  Append(set_position.inputs, position);
  Append(tree.nodes, set_position);

  blender::bNodeLink event_to_set_position = {};
  InitLink(event_to_set_position, on_update, on_update_pulse, set_position, stale_flow);
  Append(tree.links, event_to_set_position);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  const LN_CompileReport &report = program->GetCompileReport();

  EXPECT_TRUE(report.HasErrors()) << FormatCompileReport(report);
  EXPECT_TRUE(HasErrorContaining(report, "not part of the native node definition"))
      << FormatCompileReport(report);
  EXPECT_EQ(FindInstruction(program->GetInstructions(LN_Event::OnFixedUpdate),
                            LN_OpCode::SetWorldPosition),
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
  InitNode(set_position, "LogicNativeSetWorldPosition", "Set Position", 3);
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(position, "Position", blender::SOCK_VECTOR, &position_default);
  Append(set_position.inputs, flow);
  Append(set_position.inputs, position);
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
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
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
  InitNode(set_position, "LogicNativeSetWorldPosition", "Set Position", 3);
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(position, "Position", blender::SOCK_VECTOR, &position_default);
  Append(set_position.inputs, flow);
  Append(set_position.inputs, position);
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
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::Print);
  ASSERT_NE(print_instruction, nullptr);
  ASSERT_LT(print_instruction->value_expr_index, program->GetValueExpressions().size());
  EXPECT_EQ(program->GetValueExpressions()[print_instruction->value_expr_index].kind,
            LN_ValueExpressionKind::FromFloat);
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
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::Print);
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
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::Print);
  ASSERT_NE(print_instruction, nullptr);
  const LN_ValueExpression *value_expr = ValueExpressionAt(*program,
                                                           print_instruction->value_expr_index);
  ASSERT_NE(value_expr, nullptr);
  EXPECT_EQ(value_expr->kind, LN_ValueExpressionKind::BonePoseRotation);
  EXPECT_EQ(value_expr->property_ref_index, uint32_t(LN_BonePoseRotationSpace::World));
  EXPECT_EQ(value_expr->input0, LN_INVALID_INDEX);
  EXPECT_NE(value_expr->string_expr_index, LN_INVALID_INDEX);
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

  const LN_BoolExpression *guard = BoolExpressionAt(*program,
                                                    print_instruction->bool_guard_expr_index);
  ASSERT_NE(guard, nullptr);
  EXPECT_EQ(guard->kind, LN_BoolExpressionKind::EventReceived);
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
  EXPECT_NE(FindInstruction(program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::Print),
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

  const LN_BoolExpression *guard = BoolExpressionAt(*program,
                                                    print_instruction->bool_guard_expr_index);
  ASSERT_NE(guard, nullptr);
  EXPECT_EQ(guard->kind, LN_BoolExpressionKind::EventReceived);
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
  EXPECT_NE(FindInstruction(program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::Print),
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
  EXPECT_EQ(
      FindInstruction(program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::SendEvent),
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

  const LN_BoolExpression *guard = BoolExpressionAt(*program,
                                                    print_instruction->bool_guard_expr_index);
  ASSERT_NE(guard, nullptr);
  EXPECT_EQ(guard->kind, LN_BoolExpressionKind::EventReceived);
  EXPECT_TRUE(guard->bool_value);
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

  const LN_Instruction *instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::SendEvent);
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

  const LN_BoolExpression *guard = BoolExpressionAt(*program,
                                                    print_instruction->bool_guard_expr_index);
  ASSERT_NE(guard, nullptr);
  EXPECT_EQ(guard->kind, LN_BoolExpressionKind::ValueChangedTo);
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

  const LN_BoolExpression *guard = BoolExpressionAt(*program,
                                                    print_instruction->bool_guard_expr_index);
  ASSERT_NE(guard, nullptr);
  EXPECT_EQ(guard->kind, LN_BoolExpressionKind::ValueChanged);
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
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::Print);
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
  for (const LN_Instruction &instruction : program->GetInstructions(LN_Event::OnFixedUpdate)) {
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
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
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
  InitSocket(collision_object,
             "Object",
             blender::SOCK_OBJECT,
             nullptr,
             "NodeSocketLogicObject");
  InitSocket(collision_property,
             "Property",
             blender::SOCK_STRING,
             nullptr,
             "NodeSocketLogicString");
  InitSocket(collision_material,
             "Material",
             blender::SOCK_MATERIAL,
             nullptr,
             "NodeSocketLogicMaterial");
  InitSocket(collision_stay,
             "On Stay",
             blender::SOCK_BOOLEAN,
             nullptr,
             "NodeSocketLogicExecution");
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
  InitSocket(print_flow,
             "Flow",
             blender::SOCK_BOOLEAN,
             nullptr,
             "NodeSocketLogicExecution");
  InitSocketWithIdentifier(print_value,
                           "Message",
                           "Value",
                           blender::SOCK_CUSTOM,
                           nullptr,
                           "NodeSocketLogicGeneric");
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
  InitSocket(on_update_pulse,
             "Out",
             blender::SOCK_BOOLEAN,
             nullptr,
             "NodeSocketLogicExecution");
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNode collision = {};
  blender::bNodeSocket collision_object = {};
  blender::bNodeSocket collision_property = {};
  blender::bNodeSocket collision_material = {};
  blender::bNodeSocket collision_colliding = {};
  InitNode(collision, "LogicNativeCollision", "Is Colliding", 2);
  InitSocket(collision_object,
             "Object",
             blender::SOCK_OBJECT,
             nullptr,
             "NodeSocketLogicObject");
  InitSocket(collision_property,
             "Property",
             blender::SOCK_STRING,
             nullptr,
             "NodeSocketLogicString");
  InitSocket(collision_material,
             "Material",
             blender::SOCK_MATERIAL,
             nullptr,
             "NodeSocketLogicMaterial");
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
  InitSocket(print_flow,
             "Flow",
             blender::SOCK_BOOLEAN,
             nullptr,
             "NodeSocketLogicExecution");
  InitSocketWithIdentifier(print_value,
                           "Message",
                           "Value",
                           blender::SOCK_CUSTOM,
                           nullptr,
                           "NodeSocketLogicGeneric");
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
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::Print);
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
  const LN_Instruction *attempt = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::TryOnce);
  ASSERT_NE(reset, nullptr);
  ASSERT_NE(attempt, nullptr);
  EXPECT_EQ(reset->int_value, attempt->int_value);
  EXPECT_EQ(FindInstruction(program->GetInstructions(LN_Event::OnInit), LN_OpCode::TryOnce),
            nullptr);

  const std::vector<LN_Instruction> &fixed = program->GetInstructions(LN_Event::OnFixedUpdate);
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
  const std::vector<LN_Instruction> &fixed = program->GetInstructions(LN_Event::OnFixedUpdate);
  const int reset_index = FindInstructionIndex(fixed, LN_OpCode::ResetOnce);
  const int attempt_index = FindInstructionIndex(fixed, LN_OpCode::TryOnce);
  ASSERT_GE(reset_index, 0);
  EXPECT_GT(attempt_index, reset_index);
  EXPECT_EQ(fixed[size_t(reset_index)].int_value, fixed[size_t(attempt_index)].int_value);
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
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 2);
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
  EXPECT_NE(FindInstruction(program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::Print),
            nullptr);
  EXPECT_EQ(FindInstruction(program->GetInstructions(LN_Event::OnInit), LN_OpCode::TryCooldown),
            nullptr);
  EXPECT_EQ(FindInstruction(program->GetInstructions(LN_Event::OnFixedUpdate),
                            LN_OpCode::TryCooldown),
            nullptr);
  EXPECT_EQ(FindInstruction(program->GetInstructions(LN_Event::OnFixedUpdate),
                            LN_OpCode::ResetCooldown),
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
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
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
    const LN_BoolExpression *condition = BoolExpressionAt(*program,
                                                          instruction.bool_expr_index);
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

  const LN_Instruction *remove_instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::RemoveObject);
  ASSERT_NE(remove_instruction, nullptr);
  EXPECT_TRUE(BoolExpressionTreeContainsKind(
      *program, remove_instruction->bool_guard_expr_index, LN_BoolExpressionKind::BarrierPassed));
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
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 2);
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
      FindInstruction(program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::UpdatePulsify),
      nullptr);

  const LN_Instruction *quit_instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::QuitGame);
  ASSERT_NE(quit_instruction, nullptr);
  EXPECT_NE(FindBranchRouteWithConditionKind(*program,
                                            program->GetInstructions(LN_Event::OnFixedUpdate),
                                            LN_BoolExpressionKind::DelayDone),
            nullptr);
  EXPECT_TRUE(BoolExpressionTreeContainsKind(*program,
                                             quit_instruction->bool_guard_expr_index,
                                             LN_BoolExpressionKind::InstructionExecuted));

  const LN_Instruction *restart_instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::RestartGame);
  ASSERT_NE(restart_instruction, nullptr);
  EXPECT_NE(FindBranchRouteWithConditionKind(*program,
                                            program->GetInstructions(LN_Event::OnFixedUpdate),
                                            LN_BoolExpressionKind::PulsifyPulse),
            nullptr);
  EXPECT_TRUE(BoolExpressionTreeContainsKind(*program,
                                             restart_instruction->bool_guard_expr_index,
                                             LN_BoolExpressionKind::InstructionExecuted));
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

TEST(LN_TreeCompiler, TweenRotationThroughSeparateEulerFeedsSetOrientation)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueRotation from_default = {};
  blender::bNodeSocketValueRotation to_default = {};
  to_default.value_euler[2] = 3.14159265f;
  blender::bNodeSocketValueFloat duration_default = {};
  duration_default.value = 1.0f;
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
  InitNode(separate, "LogicNativeSeparateEuler", "Separate Euler", 3);
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
  InitNode(euler, "LogicNativeEuler", "Euler", 4);
  InitSocket(euler_x, "X", blender::SOCK_FLOAT, nullptr);
  InitSocket(euler_y, "Y", blender::SOCK_FLOAT, nullptr);
  InitSocket(euler_z, "Z", blender::SOCK_FLOAT, nullptr);
  InitSocketWithIdentifier(euler_rotation, "Rotation", "Euler", blender::SOCK_ROTATION, nullptr);
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
  blender::bNodeSocket done = {};
  InitNode(set_orientation, "LogicNativeSetObjectAttribute", "Set Attribute", 5);
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

  const LN_FloatExpression *z_component = FloatExpressionAt(*program, combined->input2);
  ASSERT_NE(z_component, nullptr);
  EXPECT_EQ(z_component->kind, LN_FloatExpressionKind::VectorComponent);
  EXPECT_EQ(z_component->component_index, 2);

  const LN_VectorExpression *tween_rotation = VectorExpressionAt(*program, z_component->input0);
  ASSERT_NE(tween_rotation, nullptr);
  EXPECT_EQ(tween_rotation->kind, LN_VectorExpressionKind::TweenRotationResult);
}

TEST(LN_TreeCompiler, EmitsCommandsChainedFromStopActionDoneOutput)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
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
  InitNode(set_position, "LogicNativeSetWorldPosition", "Set Position", 3);
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(position, "Position", blender::SOCK_VECTOR, &position_default);
  Append(set_position.inputs, flow);
  Append(set_position.inputs, position);
  Append(tree.nodes, set_position);

  blender::bNodeLink event_to_stop = {};
  blender::bNodeLink stop_done_to_position = {};
  InitLink(event_to_stop, on_update, on_update_pulse, stop_action, stop_flow);
  InitLink(stop_done_to_position, stop_action, stop_done, set_position, flow);
  Append(tree.links, event_to_stop);
  Append(tree.links, stop_done_to_position);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors());
  EXPECT_NE(
      nullptr,
      FindInstruction(program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::StopAction));
  EXPECT_TRUE(HasVectorInstruction(program->GetInstructions(LN_Event::OnFixedUpdate),
                                   LN_OpCode::SetTransformVector,
                                   MT_Vector3(0.25f, 0.5f, 0.75f)));
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
  template_default.value = &template_object;
  transform_default.value = &transform_object;
  life_default.value = 0.0f;
  full_copy_default.value = 0;

  auto init_add_object_node = [&](blender::bNode &node,
                                  blender::bNodeSocket &flow,
                                  blender::bNodeSocket &object_to_add,
                                  blender::bNodeSocket &copy_transform,
                                  blender::bNodeSocket &life,
                                  blender::bNodeSocket &full_copy,
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
    InitSocket(done, "Done", blender::SOCK_BOOLEAN, nullptr);
    InitSocket(added_object, "Added Object", blender::SOCK_OBJECT, nullptr);
    SetCString(done.identifier, "Out");
    SetCString(added_object.identifier, "Object");
    Append(node.inputs, flow);
    Append(node.inputs, object_to_add);
    Append(node.inputs, copy_transform);
    Append(node.inputs, life);
    Append(node.inputs, full_copy);
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
  blender::bNodeSocket add_a_done = {};
  blender::bNodeSocket add_a_added_object = {};
  init_add_object_node(add_object_a,
                       add_a_flow,
                       add_a_object,
                       add_a_transform,
                       add_a_life,
                       add_a_full_copy,
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
  blender::bNodeSocket add_b_done = {};
  blender::bNodeSocket add_b_added_object = {};
  init_add_object_node(add_object_b,
                       add_b_flow,
                       add_b_object,
                       add_b_transform,
                       add_b_life,
                       add_b_full_copy,
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
  ASSERT_EQ(instructions.size(), 5u) << opcode_sequence;
  EXPECT_EQ(instructions[0].opcode, LN_OpCode::Nop);
  EXPECT_EQ(instructions[1].opcode, LN_OpCode::AddObject);
  EXPECT_EQ(instructions[2].opcode, LN_OpCode::SetGameProperty);
  EXPECT_EQ(instructions[3].opcode, LN_OpCode::AddObject);
  EXPECT_EQ(instructions[4].opcode, LN_OpCode::SetGameProperty);
  EXPECT_NE(instructions[1].property_ref_index, LN_INVALID_INDEX);
  EXPECT_NE(instructions[3].property_ref_index, LN_INVALID_INDEX);

  const std::vector<LN_SourceRef> &source_refs = program->GetSourceRefs();
  ASSERT_LT(instructions[1].source_ref_index, source_refs.size());
  ASSERT_LT(instructions[2].source_ref_index, source_refs.size());
  ASSERT_LT(instructions[3].source_ref_index, source_refs.size());
  ASSERT_LT(instructions[4].source_ref_index, source_refs.size());
  EXPECT_EQ(source_refs[instructions[1].source_ref_index].node_name, "Add Object A");
  EXPECT_EQ(source_refs[instructions[2].source_ref_index].node_name, "Set ID A");
  EXPECT_EQ(source_refs[instructions[3].source_ref_index].node_name, "Add Object B");
  EXPECT_EQ(source_refs[instructions[4].source_ref_index].node_name, "Set ID B");
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
  template_default.value = &template_object;
  transform_default.value = &transform_object;
  life_default.value = 0.0f;
  full_copy_default.value = 0;

  blender::bNode add_object = {};
  blender::bNodeSocket add_flow = {};
  blender::bNodeSocket add_object_to_add = {};
  blender::bNodeSocket add_copy_transform = {};
  blender::bNodeSocket add_life = {};
  blender::bNodeSocket add_full_copy = {};
  blender::bNodeSocket add_done = {};
  blender::bNodeSocket add_added_object = {};
  InitNode(add_object, "LogicNativeAddObject", "Add Object", 2);
  InitSocket(add_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(add_object_to_add, "Object to Add", blender::SOCK_OBJECT, &template_default);
  InitSocket(add_copy_transform, "Copy Transform", blender::SOCK_OBJECT, &transform_default);
  InitSocket(add_life, "Life", blender::SOCK_FLOAT, &life_default);
  InitSocket(add_full_copy, "Full Copy", blender::SOCK_BOOLEAN, &full_copy_default);
  InitSocket(add_done, "Done", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(add_added_object, "Added Object", blender::SOCK_OBJECT, nullptr);
  SetCString(add_done.identifier, "Out");
  SetCString(add_added_object.identifier, "Object");
  Append(add_object.inputs, add_flow);
  Append(add_object.inputs, add_object_to_add);
  Append(add_object.inputs, add_copy_transform);
  Append(add_object.inputs, add_life);
  Append(add_object.inputs, add_full_copy);
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
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
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
  InitNode(set_position, "LogicNativeSetWorldPosition", "Set Position", 3);
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(position, "Position", blender::SOCK_VECTOR, &position_default);
  Append(set_position.inputs, flow);
  Append(set_position.inputs, position);
  Append(tree.nodes, set_position);

  blender::bNodeLink event_to_stop = {};
  blender::bNodeLink done_to_position = {};
  InitLink(event_to_stop, on_update, on_update_pulse, stop_sounds, stop_condition);
  InitLink(done_to_position, stop_sounds, stop_done, set_position, flow);
  Append(tree.links, event_to_stop);
  Append(tree.links, done_to_position);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors());
  EXPECT_NE(nullptr,
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
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
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
  InitNode(set_position, "LogicNativeSetWorldPosition", "Set Position", 3);
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(position, "Position", blender::SOCK_VECTOR, &position_default);
  Append(set_position.inputs, flow);
  Append(set_position.inputs, position);
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
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
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
  InitNode(set_position, "LogicNativeSetWorldPosition", "Set Position", 4);
  InitSocket(set_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(position, "Position", blender::SOCK_VECTOR, &position_default);
  Append(set_position.inputs, set_flow);
  Append(set_position.inputs, position);
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
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
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
  const LN_Instruction *instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::SetMaterialParameter);
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
  const LN_Instruction *instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::SetMaterialParameter);
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
  InitNode(set_modifier_input,
           "LogicNativeSetGeometryNodesInput",
           "Set Geometry Nodes Input",
           2);
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
  InitLink(modifier_flow_link,
           on_update,
           on_update_pulse,
           set_modifier_input,
           modifier_flow);
  Append(tree.links, modifier_flow_link);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  const std::vector<LN_Instruction> &instructions = program->GetInstructions(
      LN_Event::OnFixedUpdate);

  const LN_Instruction *modifier_instruction = FindInstruction(
      instructions, LN_OpCode::SetGeometryNodesInput);
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
  const LN_FloatExpression *compiled_modifier_value = FloatExpressionAt(
      *program, modifier_value->input0);
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
  InitSocketWithIdentifier(
      getter_node, "Node Name", "Node", blender::SOCK_STRING, &node_default);
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
  InitSocketWithIdentifier(
      setter_node, "Node Name", "Node", blender::SOCK_STRING, &node_default);
  InitSocket(setter_socket, "Socket", blender::SOCK_STRING, &socket_default);
  Append(setter.inputs, setter_flow);
  Append(setter.inputs, setter_object);
  Append(setter.inputs, setter_slot);
  Append(setter.inputs, setter_modifier);
  Append(setter.inputs, setter_node);
  Append(setter.inputs, setter_socket);
  AppendMaterialParameterValueSockets(
      setter, setter_values, &setter_float_default, nullptr);
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
  const std::vector<LN_Instruction> &instructions = program->GetInstructions(
      LN_Event::OnFixedUpdate);

  const LN_Instruction *unique_instruction = FindInstruction(
      instructions, LN_OpCode::MakeNodeTreeUnique);
  ASSERT_NE(unique_instruction, nullptr);
  EXPECT_EQ(unique_instruction->int_value, 2);

  const LN_Instruction *setter_instruction = FindInstruction(
      instructions, LN_OpCode::SetGeometryNodeSocketValue);
  ASSERT_NE(setter_instruction, nullptr);
  const LN_ValueExpression *setter_value = ValueExpressionAt(
      *program, setter_instruction->secondary_value_expr_index);
  ASSERT_NE(setter_value, nullptr);
  EXPECT_EQ(setter_value->kind, LN_ValueExpressionKind::EditorNodeValue);

  const LN_Instruction *consumer_instruction = FindInstruction(
      instructions, LN_OpCode::SetGameProperty);
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
  InitNode(modifier_node,
           "LogicNativeEnableDisableModifier",
           "Enable or Disable Modifier",
           2);
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
  const LN_Instruction *instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::EnableDisableModifier);
  ASSERT_NE(instruction, nullptr);
  EXPECT_EQ(instruction->int_value, 2);
  EXPECT_EQ(instruction->string_expr_index, LN_INVALID_INDEX);
  EXPECT_EQ(instruction->int_expr_index, LN_INVALID_INDEX);
  const LN_BoolExpression *enabled_expr = BoolExpressionAt(*program,
                                                           instruction->bool_expr_index);
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
  InitNode(append_node,
           "LogicNativeAssignGeometryNodesModifier",
           "Append Geometry Nodes Modifier",
           2);
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
  InitNode(insert_node,
           "LogicNativeAssignGeometryNodesModifier",
           "Insert Geometry Nodes Modifier",
           3);
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
  InitNode(enable_node,
           "LogicNativeEnableDisableModifier",
           "Disable Assigned Modifier",
           5);
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

  const std::vector<LN_Instruction> &instructions = program->GetInstructions(
      LN_Event::OnFixedUpdate);
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

  const LN_Instruction *enable_instruction = FindInstruction(
      instructions, LN_OpCode::EnableDisableModifier);
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
  InitNode(assign_node,
           "LogicNativeAssignGeometryNodesModifier",
           "Assign Geometry Nodes Modifier",
           2);
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
  EXPECT_EQ(FindInstruction(program->GetInstructions(LN_Event::OnFixedUpdate),
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
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::Print);
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
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
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

TEST(LN_TreeCompiler, EmitsSetLinearVelocityFromOnUpdate)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
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
  InitNode(set_velocity, "LogicNativeSetLinearVelocity", "Set Velocity", 2);
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(velocity, "Velocity", blender::SOCK_VECTOR, &velocity_default);
  Append(set_velocity.inputs, flow);
  Append(set_velocity.inputs, velocity);
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
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
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
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 2);
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

TEST(LN_TreeCompiler, RejectsSetLinearVelocityFromValueBoolFlow)
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
  InitNode(set_velocity, "LogicNativeSetLinearVelocity", "Set Velocity", 2);
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(velocity, "Velocity", blender::SOCK_VECTOR, &velocity_default);
  Append(set_velocity.inputs, flow);
  Append(set_velocity.inputs, velocity);
  Append(tree.nodes, set_velocity);

  blender::bNodeLink value_to_flow = {};
  InitLink(value_to_flow, value, value_output, set_velocity, flow);
  Append(tree.links, value_to_flow);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_TRUE(program->GetCompileReport().HasErrors());
  EXPECT_TRUE(HasErrorContaining(program->GetCompileReport(), "requires an execution output"))
      << FormatCompileReport(program->GetCompileReport());
  const LN_Instruction *instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::SetVelocityVector);
  EXPECT_EQ(instruction, nullptr);
}

TEST(LN_TreeCompiler, EmitsSetWorldPositionFromSnapshotWorldPosition)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNode get_position = {};
  blender::bNodeSocket get_position_output = {};
  InitNode(get_position, "LogicNativeGetWorldPosition", "Get Position", 2);
  InitSocket(get_position_output, "Position", blender::SOCK_VECTOR, nullptr);
  Append(get_position.outputs, get_position_output);
  Append(tree.nodes, get_position);

  blender::bNodeSocketValueVector position_default = {};
  blender::bNode set_position = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket position = {};
  InitNode(set_position, "LogicNativeSetWorldPosition", "Set Position", 3);
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(position, "Position", blender::SOCK_VECTOR, &position_default);
  Append(set_position.inputs, flow);
  Append(set_position.inputs, position);
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
  EXPECT_EQ(expression->kind, LN_VectorExpressionKind::SnapshotWorldPosition);
}

TEST(LN_TreeCompiler, EmitsSetLinearVelocityFromSnapshotLinearVelocity)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNode get_velocity = {};
  blender::bNodeSocket get_velocity_output = {};
  InitNode(get_velocity, "LogicNativeGetLinearVelocity", "Get Velocity", 2);
  InitSocket(get_velocity_output, "Velocity", blender::SOCK_VECTOR, nullptr);
  Append(get_velocity.outputs, get_velocity_output);
  Append(tree.nodes, get_velocity);

  blender::bNodeSocketValueVector velocity_default = {};
  blender::bNode set_velocity = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket velocity = {};
  InitNode(set_velocity, "LogicNativeSetLinearVelocity", "Set Velocity", 3);
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(velocity, "Velocity", blender::SOCK_VECTOR, &velocity_default);
  Append(set_velocity.inputs, flow);
  Append(set_velocity.inputs, velocity);
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
  EXPECT_EQ(expression->kind, LN_VectorExpressionKind::SnapshotLinearVelocity);
}

TEST(LN_TreeCompiler, EmitsVectorMathExpressionForSetWorldPosition)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNode get_position = {};
  blender::bNodeSocket get_position_output = {};
  InitNode(get_position, "LogicNativeGetWorldPosition", "Get Position", 2);
  InitSocket(get_position_output, "Position", blender::SOCK_VECTOR, nullptr);
  Append(get_position.outputs, get_position_output);
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
  InitNode(set_position, "LogicNativeSetWorldPosition", "Set Position", 5);
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(position, "Position", blender::SOCK_VECTOR, &position_default);
  Append(set_position.inputs, flow);
  Append(set_position.inputs, position);
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
  EXPECT_EQ(left->kind, LN_VectorExpressionKind::SnapshotWorldPosition);
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
  InitNode(set_position, "LogicNativeSetWorldPosition", "Set Position", 3);
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(position, "Position", blender::SOCK_VECTOR, &position_default);
  Append(set_position.inputs, flow);
  Append(set_position.inputs, position);
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
  InitNode(set_position, "LogicNativeSetWorldPosition", "Set Position", 3);
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(position, "Position", blender::SOCK_VECTOR, &position_default);
  Append(set_position.inputs, flow);
  Append(set_position.inputs, position);
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
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
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
  InitNode(set_position, "LogicNativeSetWorldPosition", "Set Position", 5);
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(position, "Position", blender::SOCK_VECTOR, &position_default);
  Append(set_position.inputs, flow);
  Append(set_position.inputs, position);
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
  InitNode(set_position, "LogicNativeSetWorldPosition", "Set Position", 2);
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(position, "Position", blender::SOCK_VECTOR, &position_default);
  Append(set_position.inputs, flow);
  Append(set_position.inputs, position);
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

TEST(LN_TreeCompiler, EmitsApplyImpulseFromOnUpdate)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
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

TEST(LN_TreeCompiler, RejectsApplyImpulseFromValueBoolFlow)
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
  EXPECT_TRUE(program->GetCompileReport().HasErrors());
  EXPECT_TRUE(HasErrorContaining(program->GetCompileReport(), "requires an execution output"))
      << FormatCompileReport(program->GetCompileReport());
  const LN_Instruction *instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::ApplyImpulse);
  EXPECT_EQ(instruction, nullptr);
}

TEST(LN_TreeCompiler, RejectsSetWorldPositionFromValueBoolFlow)
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
  InitNode(set_position, "LogicNativeSetWorldPosition", "Set Position", 2);
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(position, "Position", blender::SOCK_VECTOR, &position_default);
  Append(set_position.inputs, flow);
  Append(set_position.inputs, position);
  Append(tree.nodes, set_position);

  blender::bNodeLink value_to_flow = {};
  InitLink(value_to_flow, value, value_output, set_position, flow);
  Append(tree.links, value_to_flow);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_TRUE(program->GetCompileReport().HasErrors());
  EXPECT_TRUE(HasErrorContaining(program->GetCompileReport(), "requires an execution output"))
      << FormatCompileReport(program->GetCompileReport());
  const LN_Instruction *instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::SetTransformVector);
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

  const LN_BoolExpression *guard = BoolExpressionAt(*program, instruction->bool_guard_expr_index);
  ASSERT_NE(guard, nullptr);
  EXPECT_EQ(guard->kind, LN_BoolExpressionKind::InputStatus);
  EXPECT_EQ(guard->int_value, int32_t(SCA_IInputDevice::SPACEKEY));
  EXPECT_EQ(guard->secondary_int_value, int32_t(SCA_InputEvent::JUSTACTIVATED));
  EXPECT_EQ(guard->string_value, "SPACE");
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

    const LN_BoolExpression *guard = BoolExpressionAt(*program,
                                                      instruction->bool_guard_expr_index);
    ASSERT_NE(guard, nullptr);
    EXPECT_EQ(guard->kind, LN_BoolExpressionKind::InputStatus);
    EXPECT_EQ(guard->int_value, int32_t(test_case.expected_code));
  }
}

TEST(LN_TreeCompiler, MouseLookCompilesMouseInputAndCenterMouseBit)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
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
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
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
  InitSocket(ray_hit_object, "Hit Object", blender::SOCK_OBJECT, nullptr);
  Append(raycast.inputs, ray_flow);
  Append(raycast.inputs, ray_caster);
  Append(raycast.inputs, ray_ignore);
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

  const LN_IntExpression *mask_expression = IntExpressionAt(*program, queries[0].int_expr_index);
  ASSERT_NE(mask_expression, nullptr);
  EXPECT_EQ(mask_expression->kind, LN_IntExpressionKind::Constant);
  EXPECT_EQ(mask_expression->int_value, 341);
}

TEST(LN_TreeCompiler, EmitsRuntimeBranchGuardFromGamePropertyBool)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
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
  InitNode(set_position, "LogicNativeSetWorldPosition", "Set Position", 4);
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(position, "Position", blender::SOCK_VECTOR, &position_default);
  Append(set_position.inputs, flow);
  Append(set_position.inputs, position);
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
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
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
  blender::bNodeSocket gate_condition_a = {};
  blender::bNodeSocket gate_condition_b = {};
  blender::bNodeSocket gate_result = {};
  InitNode(gate, "LogicNativeGate", "Gate", 4);
  gate.custom1 = 1;
  InitSocket(gate_condition_a, "Condition A", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(gate_condition_b, "Condition B", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(gate_result, "Result", blender::SOCK_BOOLEAN, nullptr);
  Append(gate.inputs, gate_condition_a);
  Append(gate.inputs, gate_condition_b);
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
  InitNode(set_position, "LogicNativeSetWorldPosition", "Set Position", 6);
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(position, "Position", blender::SOCK_VECTOR, &position_default);
  Append(set_position.inputs, flow);
  Append(set_position.inputs, position);
  Append(tree.nodes, set_position);

  blender::bNodeLink event_to_branch = {};
  blender::bNodeLink property_a_to_gate = {};
  blender::bNodeLink property_b_to_gate = {};
  blender::bNodeLink gate_to_branch = {};
  blender::bNodeLink branch_to_set_position = {};
  InitLink(event_to_branch, on_update, on_update_pulse, branch, branch_flow);
  InitLink(property_a_to_gate, get_property_a, property_a_value, gate, gate_condition_a);
  InitLink(property_b_to_gate, get_property_b, property_b_value, gate, gate_condition_b);
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

  const LN_FloatExpression *clamp = FloatExpressionAt(*program, instruction->float_expr_index);
  ASSERT_NE(clamp, nullptr);
  EXPECT_EQ(clamp->kind, LN_FloatExpressionKind::Clamp);
  const LN_FloatExpression *multiply = FloatExpressionAt(*program, clamp->input0);
  ASSERT_NE(multiply, nullptr);
  EXPECT_EQ(multiply->kind, LN_FloatExpressionKind::Multiply);
  const LN_FloatExpression *current = FloatExpressionAt(*program, multiply->input0);
  ASSERT_NE(current, nullptr);
  EXPECT_EQ(current->kind, LN_FloatExpressionKind::SnapshotGameProperty);
  const LN_FloatExpression *delta = FloatExpressionAt(*program, multiply->input1);
  ASSERT_NE(delta, nullptr);
  EXPECT_EQ(delta->kind, LN_FloatExpressionKind::Constant);
  EXPECT_FLOAT_EQ(delta->float_value, 2.0f);
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

  const LN_FloatExpression *floor = FloatExpressionAt(*program, instruction->float_expr_index);
  ASSERT_NE(floor, nullptr);
  EXPECT_EQ(floor->kind, LN_FloatExpressionKind::Floor);
  const LN_FloatExpression *divide = FloatExpressionAt(*program, floor->input0);
  ASSERT_NE(divide, nullptr);
  EXPECT_EQ(divide->kind, LN_FloatExpressionKind::Divide);
  const LN_FloatExpression *current = FloatExpressionAt(*program, divide->input0);
  ASSERT_NE(current, nullptr);
  EXPECT_EQ(current->kind, LN_FloatExpressionKind::SnapshotGameProperty);
  const LN_FloatExpression *delta = FloatExpressionAt(*program, divide->input1);
  ASSERT_NE(delta, nullptr);
  EXPECT_EQ(delta->kind, LN_FloatExpressionKind::Constant);
  EXPECT_FLOAT_EQ(delta->float_value, 3.0f);
}

TEST(LN_TreeCompiler, ModifyPropertyRejectsUnsupportedAttributeMode)
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
  EXPECT_TRUE(program->GetCompileReport().HasErrors());
  EXPECT_TRUE(HasErrorContaining(program->GetCompileReport(), "Attribute mode"))
      << FormatCompileReport(program->GetCompileReport());
}

TEST(LN_TreeCompiler, EmitsVectorScaleFromGamePropertyFloat)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
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
  InitNode(set_position, "LogicNativeSetWorldPosition", "Set Position", 5);
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(position, "Position", blender::SOCK_VECTOR, &position_default);
  Append(set_position.inputs, flow);
  Append(set_position.inputs, position);
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
  blender::LogicNodeBinding binding = {};
  SetCString(binding.tree_name, "LogicProbe");
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
}

TEST(LN_GamePropertyBindingSource, PreservesDisabledNativeBinding)
{
  blender::Object object = {};
  blender::LogicNodeBinding binding = {};
  SetCString(binding.tree_name, "LogicProbe");
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
  blender::LogicNodeBinding active_binding = {};
  blender::LogicNodeBinding active_second_binding = {};
  blender::LogicNodeBinding inactive_binding = {};
  SetCString(active_binding.tree_name, "ActiveTree");
  SetCString(active_second_binding.tree_name, "ActiveTreeSecond");
  SetCString(inactive_binding.tree_name, "InactiveTree");
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
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
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
  InitNode(set_position, "LogicNativeSetWorldPosition", "Set Position", 3);
  InitSocket(set_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(set_position_socket, "Position", blender::SOCK_VECTOR, &position_default);
  Append(set_position.inputs, set_flow);
  Append(set_position.inputs, set_position_socket);
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
}

TEST(LN_TreeCompiler, EmitsStopSoundOpcode)
{
  blender::bNodeTree tree = {};
  InitTree(tree);

  blender::ID sound_id = {};
  init_sound_id_for_tests(sound_id, "Coin");

  blender::bNode on_update = {};
  blender::bNodeSocket on_update_pulse = {};
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
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
  InitNode(set_position, "LogicNativeSetWorldPosition", "Set Position", 3);
  InitSocket(set_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(set_position_socket, "Position", blender::SOCK_VECTOR, &position_default);
  Append(set_position.inputs, set_flow);
  Append(set_position.inputs, set_position_socket);
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
  blender::bNodeSocket play_done = {};
  InitNode(play_3d, "LogicNativePlaySound3D", "Play 3D", 2);
  play_3d.id = &sound_id;
  InitSocket(play_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(play_volume, "Volume", blender::SOCK_FLOAT, &volume_default);
  InitSocket(play_pitch, "Pitch", blender::SOCK_FLOAT, &pitch_default);
  InitSocket(play_loop, "Loop", blender::SOCK_BOOLEAN, &loop_default);
  InitSocket(play_done, "Done", blender::SOCK_BOOLEAN, nullptr);
  Append(play_3d.inputs, play_flow);
  Append(play_3d.inputs, play_volume);
  Append(play_3d.inputs, play_pitch);
  Append(play_3d.inputs, play_loop);
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
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::SetGameProperty);
  ASSERT_NE(set_game_instruction, nullptr);
  const LN_FloatExpression *float_expr = FloatExpressionAt(*program,
                                                           set_game_instruction->float_expr_index);
  ASSERT_NE(float_expr, nullptr);
  EXPECT_EQ(float_expr->kind, LN_FloatExpressionKind::FromGenericValue);
  const LN_ValueExpression *value_expr = ValueExpressionAt(*program, float_expr->input0);
  ASSERT_NE(value_expr, nullptr);
  EXPECT_EQ(value_expr->kind, LN_ValueExpressionKind::RuntimeTreeProperty);
}

TEST(LN_TreeCompiler, EmitsSetWorldPositionFromBoneHeadWorld)
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
  InitNode(set_position, "LogicNativeSetWorldPosition", "Set Position", 3);
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(position, "Position", blender::SOCK_VECTOR, &position_default);
  Append(set_position.inputs, flow);
  Append(set_position.inputs, position);
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

TEST(LN_TreeCompiler, LegacyBoneCenterPoseChannelCompilesAsArmatureSpace)
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
  blender::bNodeSocket bone_position = {};
  InitNode(get_bone, "LogicNativeGetBoneCenterPoseWorld", "Bone Center Pose", 2);
  get_bone.custom1 = 2;
  InitSocket(bone_object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(bone_name, "Bone Name", blender::SOCK_STRING, &bone_name_default);
  InitSocket(bone_position, "Position", blender::SOCK_VECTOR, nullptr);
  Append(get_bone.inputs, bone_object);
  Append(get_bone.inputs, bone_name);
  Append(get_bone.outputs, bone_position);
  Append(tree.nodes, get_bone);

  blender::bNodeSocketValueString target_bone_name_default = {};
  SetCString(target_bone_name_default.value, "Target");
  blender::bNodeSocketValueVector location_default = {};
  blender::bNode set_bone = {};
  blender::bNodeSocket flow = {};
  blender::bNodeSocket target_object = {};
  blender::bNodeSocket target_bone_name = {};
  blender::bNodeSocket location = {};
  InitNode(set_bone, "LogicNativeSetBonePoseLocation", "Set Bone Pose Location", 3);
  InitSocket(flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(target_object, "Object", blender::SOCK_OBJECT, nullptr);
  InitSocket(target_bone_name, "Bone Name", blender::SOCK_STRING, &target_bone_name_default);
  InitSocket(location, "Location", blender::SOCK_VECTOR, &location_default);
  Append(set_bone.inputs, flow);
  Append(set_bone.inputs, target_object);
  Append(set_bone.inputs, target_bone_name);
  Append(set_bone.inputs, location);
  Append(tree.nodes, set_bone);

  blender::bNodeLink flow_link = {};
  blender::bNodeLink location_link = {};
  InitLink(flow_link, on_update, on_update_pulse, set_bone, flow);
  InitLink(location_link, get_bone, bone_position, set_bone, location);
  Append(tree.links, flow_link);
  Append(tree.links, location_link);

  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree);
  EXPECT_FALSE(program->GetCompileReport().HasErrors())
      << FormatCompileReport(program->GetCompileReport());
  const LN_Instruction *instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::SetBonePoseLocation);
  ASSERT_NE(instruction, nullptr);
  const LN_VectorExpression *expression = InstructionVectorExpression(*program, *instruction);
  ASSERT_NE(expression, nullptr);
  EXPECT_EQ(expression->kind, LN_VectorExpressionKind::BoneCenterPoseWorld);
  EXPECT_EQ(expression->property_ref_index, uint32_t(LN_BonePosePositionSpace::Armature));
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
  const LN_Instruction *instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::SetBonePoseLocation);
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
  const LN_Instruction *instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::SetBonePoseRotation);
  ASSERT_NE(instruction, nullptr);
  EXPECT_EQ(instruction->secondary_int_value, 1);

  const LN_VectorExpression *expression = InstructionVectorExpression(*program, *instruction);
  ASSERT_NE(expression, nullptr);
  EXPECT_EQ(expression->vector_value.y(), 0.25f);
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
  const LN_Instruction *instruction = FindInstruction(
      program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::SetBonePoseTransform);
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
  EXPECT_EQ(rotation_expression->vector_value.y(), 0.5f);
}

TEST(LN_TreeCompiler, RejectsLoopScopedCooldownFlowWithActionableDiagnostic)
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
  blender::bNodeSocket loop_out = {};
  InitNode(loop, "LogicNativeLoop", "Loop", 2);
  InitSocket(loop_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(loop_count, "Count", blender::SOCK_INT, &count_default);
  InitSocket(loop_out, "Loop", blender::SOCK_BOOLEAN, nullptr);
  Append(loop.inputs, loop_flow);
  Append(loop.inputs, loop_count);
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
  EXPECT_EQ(
      FindInstruction(program->GetInstructions(LN_Event::OnFixedUpdate), LN_OpCode::TryCooldown),
      nullptr);
}

TEST(LN_TreeCompiler, RoutedDoOncePulseUsesOnlyLoopActiveForTheLoopBody)
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

  blender::bNodeSocketValueInt count_default = {};
  count_default.value = 3;
  blender::bNode loop = {};
  blender::bNodeSocket loop_flow = {};
  blender::bNodeSocket loop_count = {};
  blender::bNodeSocket loop_out = {};
  InitNode(loop, "LogicNativeLoop", "Loop", 3);
  InitSocket(loop_flow, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(loop_count, "Count", blender::SOCK_INT, &count_default);
  InitSocket(loop_out, "Loop", blender::SOCK_BOOLEAN, nullptr);
  Append(loop.inputs, loop_flow);
  Append(loop.inputs, loop_count);
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
  InitNode(on_update, "LogicNativeOnUpdate", "On Update", 1);
  InitSocket(on_update_pulse, "Out", blender::SOCK_BOOLEAN, nullptr);
  Append(on_update.outputs, on_update_pulse);
  Append(tree.nodes, on_update);

  blender::bNodeSocketValueInt count_default = {};
  count_default.value = 2;
  blender::bNode loop = {};
  blender::bNodeSocket loop_condition = {};
  blender::bNodeSocket loop_count = {};
  blender::bNodeSocket loop_out = {};
  InitNode(loop, "LogicNativeLoop", "Loop", 2);
  InitSocket(loop_condition, "Flow", blender::SOCK_BOOLEAN, nullptr);
  InitSocket(loop_count, "Count", blender::SOCK_INT, &count_default);
  InitSocket(loop_out, "Loop", blender::SOCK_BOOLEAN, nullptr);
  Append(loop.inputs, loop_condition);
  Append(loop.inputs, loop_count);
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

}  // namespace
