/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_TreeCompiler_internal.hh
 *  \ingroup logicnodes
 *
 * Shared types and declarations for the logic tree compiler (split across .cc files).
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "DNA_ID.h"
#include "DNA_node_types.h"

#include "MT_Vector4.h"

#include "SCA_IInputDevice.h"

#include "LN_NodeRegistry.h"
#include "LN_Program.h"
#include "LN_TreeCompiler.h"
#include "LN_Types.h"

namespace blender {
struct bNode;
struct bNodeLink;
struct bNodeSocket;
struct bNodeTree;
}  // namespace blender

namespace ln_compiler {

using NodeDefinitionMap = std::unordered_map<const blender::bNode *, const LN_NodeDefinition *>;

struct ResolvedLink {
  const blender::bNode *fromnode = nullptr;
  const blender::bNodeSocket *fromsock = nullptr;
  const blender::bNode *tonode = nullptr;
  const blender::bNodeSocket *tosock = nullptr;
};

struct ResolvedEndpoint {
  const blender::bNode *node = nullptr;
  const blender::bNodeSocket *socket = nullptr;
};

using RawInputLinkMap =
    std::unordered_map<const blender::bNodeSocket *, std::vector<const blender::bNodeLink *>>;
using RawOutputLinkMap =
    std::unordered_map<const blender::bNodeSocket *, std::vector<const blender::bNodeLink *>>;
using InputLinkMap = std::unordered_map<const blender::bNodeSocket *, std::vector<ResolvedLink>>;
using EdgeMap = std::unordered_map<const blender::bNode *, std::vector<const blender::bNode *>>;
using ValueCache = std::unordered_map<const blender::bNodeSocket *, std::optional<LN_Value>>;
using BoolExpressionCache = std::unordered_map<const blender::bNodeSocket *,
                                               std::optional<uint32_t>>;
using IntExpressionCache = std::unordered_map<const blender::bNodeSocket *,
                                              std::optional<uint32_t>>;
using FloatExpressionCache = std::unordered_map<const blender::bNodeSocket *,
                                                std::optional<uint32_t>>;
using StringExpressionCache = std::unordered_map<const blender::bNodeSocket *,
                                                 std::optional<uint32_t>>;
using VectorExpressionCache = std::unordered_map<const blender::bNodeSocket *,
                                                 std::optional<uint32_t>>;
using ColorExpressionCache = std::unordered_map<const blender::bNodeSocket *,
                                                std::optional<uint32_t>>;
using ValueExpressionCache = std::unordered_map<const blender::bNodeSocket *,
                                                std::optional<uint32_t>>;

using CompileHandlerDescriptor = LN_TreeCompiler::CompileHandlerDescriptor;
using CompileHandlerKind = LN_TreeCompiler::CompileHandlerKind;

enum class CompileActionKind : uint8_t {
  None = 0,
  EmitEventSource,
  EmitConstantOutput,
  Custom,
};

enum class FlowResolverKind : uint8_t {
  Invalid = 0,
  EventSource,
  Branch,
  PollingEventRoute,
  FlowConditionRoute,
  LatentCompletionRoute,
  PrecompiledRoute,
  Loop,
};

struct LoopFrameCache {
  std::unordered_map<const blender::bNode *, uint32_t> indices;
  std::unordered_map<const blender::bNode *, uint32_t> index_expressions;
  std::unordered_map<const blender::bNode *, uint32_t> value_expressions;
};

extern LoopFrameCache *g_active_loop_frame_cache;

inline constexpr float LN_PI = 3.14159265358979323846f;
inline constexpr int16_t LN_STRING_OP_JOIN = 0;
inline constexpr int16_t LN_STRING_OP_CONTAINS = 2;
inline constexpr int16_t LN_STRING_OP_COUNT = 3;
inline constexpr int16_t LN_STRING_OP_REPLACE = 4;
inline constexpr int16_t LN_STRING_OP_STARTS_WITH = 5;
inline constexpr int16_t LN_STRING_OP_ENDS_WITH = 6;
inline constexpr int16_t LN_STRING_OP_UPPER = 7;
inline constexpr int16_t LN_STRING_OP_LOWER = 8;
inline constexpr int16_t LN_STRING_OP_ZFILL = 9;

struct CompilerContext;

/** Nodes on an execution chain from event/sensor roots, plus upstream data providers. */
struct ActiveNodeSet {
  std::unordered_set<const blender::bNode *> flow_reachable;
  std::unordered_set<const blender::bNode *> active;
};

bool IsExecutionFlowLink(const ResolvedLink &link, const NodeDefinitionMap &node_definitions);

ActiveNodeSet ComputeActiveNodes(const std::vector<const blender::bNode *> &nodes,
                                 const NodeDefinitionMap &node_definitions,
                                 const InputLinkMap &input_links,
                                 const std::vector<ResolvedLink> &resolved_links);

bool IsNodeActive(const ActiveNodeSet &active_nodes, const blender::bNode *node);

void WarnInactiveNodes(LN_Program &program,
                       const blender::bNodeTree &tree,
                       const std::vector<const blender::bNode *> &nodes,
                       const NodeDefinitionMap &node_definitions,
                       const ActiveNodeSet &active_nodes);

struct InternalCompileHandler {
  CompileHandlerDescriptor descriptor;
  CompileActionKind action_kind = CompileActionKind::None;
  FlowResolverKind flow_kind = FlowResolverKind::Invalid;
  LN_Event source_event = LN_Event::OnInit;
  void (*compile_fn)(CompilerContext &) = nullptr;
};

const InternalCompileHandler *FindInternalCompileHandler(LN_NodeKind kind);

inline constexpr uint64_t FNV_OFFSET_BASIS = 14695981039346656037ull;
inline constexpr uint64_t FNV_PRIME = 1099511628211ull;

std::string SafeString(const char *value);
std::string IDNameWithoutPrefix(const blender::ID &id);
std::string BuildChecksum(const blender::bNodeTree &tree);

bool IsUsedLink(const blender::bNodeLink &link);
bool IsFrameNode(const blender::bNode &node);
bool IsRerouteNode(const blender::bNode &node);

std::optional<ResolvedEndpoint> ResolveSourceEndpoint(
    const blender::bNode &node,
    const blender::bNodeSocket &socket,
    const RawInputLinkMap &raw_input_links,
    std::unordered_set<const blender::bNode *> &visited);

void AppendTargetEndpoints(const blender::bNode &node,
                           const blender::bNodeSocket &socket,
                           const RawOutputLinkMap &raw_output_links,
                           std::vector<ResolvedEndpoint> &targets,
                           std::unordered_set<const blender::bNode *> &visited);

uint32_t AddSourceRef(LN_Program &program,
                      const blender::bNodeTree &tree,
                      const blender::bNode &node,
                      const char *socket_name);

void AddNodeIssue(LN_Program &program,
                  const blender::bNodeTree &tree,
                  const blender::bNode &node,
                  const char *socket_name,
                  LN_CompileSeverity severity,
                  const std::string &message);

bool IsSupportedLogicMathOperation(int16_t operation);
bool IsSupportedLogicVectorMathOperation(int16_t operation);
bool IsSupportedLogicStringOperation(int16_t operation);

int32_t IntExpressionConstantFallback(const LN_Program &program, uint32_t expression_index);
MT_Vector4 ColorExpressionConstantFallback(const LN_Program &program, uint32_t expression_index);

/* Socket / link helpers */
bool LogicPinTypesCompatible(LN_ValueType from_type, LN_ValueType to_type);
bool LogicPinsCompatible(const LN_PinDefinition &from_pin, const LN_PinDefinition &to_pin);
std::string AddObjectResultPropertyName(const blender::bNode &node);
std::string AssignGeometryNodesModifierResultPropertyName(const blender::bNode &node);
bool NamesMatch(const char *socket_name, const char *identifier, const std::string &name);
const blender::bNodeSocket *FindInputSocket(const blender::bNode &node, const std::string &name);
const blender::bNodeSocket *FindInputSocketExact(const blender::bNode &node,
                                                 const std::string &name);
const blender::bNodeSocket *FindOutputSocket(const blender::bNode &node, const std::string &name);

inline const blender::bNodeSocket *FindInputSocket(const blender::bNode &node, const char *name)
{
  return FindInputSocket(node, std::string(name));
}

inline const blender::bNodeSocket *FindOutputSocket(const blender::bNode &node, const char *name)
{
  return FindOutputSocket(node, std::string(name));
}
const LN_PinDefinition *FindPinDefinition(const std::vector<LN_PinDefinition> &pins,
                                          const blender::bNodeSocket &socket);
const LN_PinDefinition *FindFirstExecutionInputPin(const LN_NodeDefinition &definition);
const LN_PinDefinition *FindFirstExecutionOutputPin(const LN_NodeDefinition &definition);
bool IsExecutionOutputSocket(const LN_NodeDefinition &definition,
                             const blender::bNodeSocket &socket);
std::optional<uint32_t> BuildPrimaryExecutionExpression(
    LN_Program &program,
    const blender::bNode &node,
    const LN_NodeDefinition &definition,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    FloatExpressionCache &float_expression_cache,
    BoolExpressionCache &bool_expression_cache);

std::optional<LN_Value> ReadSocketDefault(const blender::bNodeSocket &socket, LN_ValueType type);
std::optional<LN_Value> ReadInputValue(const blender::bNode &node,
                                       const std::string &socket_name,
                                       LN_ValueType expected_type,
                                       const NodeDefinitionMap &node_definitions,
                                       const InputLinkMap &input_links,
                                       ValueCache &value_cache);

std::optional<LN_Value> EvaluateOutputValue(const blender::bNode &node,
                                              const blender::bNodeSocket &output_socket,
                                              const NodeDefinitionMap &node_definitions,
                                              const InputLinkMap &input_links,
                                              ValueCache &value_cache);

/* Constant / expression builders */
uint32_t AddConstantBoolExpression(LN_Program &program, bool value);
uint32_t AddConstantIntExpression(LN_Program &program, int32_t value);
uint32_t AddConstantFloatExpression(LN_Program &program, float value);
uint32_t AddConstantStringExpression(LN_Program &program, const std::string &value);
uint32_t AddConstantColorExpression(LN_Program &program, const MT_Vector4 &value);
uint32_t AddConstantVectorExpression(LN_Program &program, const MT_Vector3 &value);
uint32_t AddConstantValueExpression(LN_Program &program, const LN_Value &value);
uint32_t AddActiveCameraValueExpression(LN_Program &program);
uint32_t AddNotBoolExpression(LN_Program &program, uint32_t input);
uint32_t AddAndBoolExpression(LN_Program &program, uint32_t input0, uint32_t input1);
uint32_t AddOrBoolExpression(LN_Program &program, uint32_t input0, uint32_t input1);
uint32_t AddXorBoolExpression(LN_Program &program, uint32_t input0, uint32_t input1);
std::optional<uint32_t> AddLogicGateBoolExpression(LN_Program &program,
                                                   int16_t operation,
                                                   uint32_t input0,
                                                   uint32_t input1);
uint32_t AddFloatCompareBoolExpression(LN_Program &program,
                                       uint32_t input0,
                                       uint32_t input1,
                                       LN_FloatCompareOperation operation);
uint32_t AddGamePropertyRef(LN_Program &program,
                            const std::string &name,
                            LN_ValueType type,
                            const LN_Value &default_value);
uint32_t AddTreePropertyRef(LN_Program &program,
                            const std::string &name,
                            LN_ValueType type,
                            const LN_Value &default_value);
uint32_t AddVectorComponentFloatExpression(LN_Program &program,
                                           uint32_t vector_expression_index,
                                           uint8_t component_index);

bool BoolExpressionConstantFallback(const LN_Program &program, uint32_t expression_index);
float FloatExpressionConstantFallback(const LN_Program &program, uint32_t expression_index);
MT_Vector3 VectorExpressionConstantFallback(const LN_Program &program, uint32_t expression_index);

LN_Value MakeNoneValue();
LN_Value MakeDefaultValue(LN_ValueType value_type);

std::optional<float> EvaluateMath(int16_t operation, float a, float b);
bool EvaluateFloatCompare(LN_FloatCompareOperation operation, float a, float b);
bool EvaluateRange(LN_RangeOperation operation, float value, float min_value, float max_value);
float EvaluateThreshold(LN_ThresholdOperation operation,
                        float value,
                        float threshold,
                        bool else_zero);

std::optional<LN_FloatCompareOperation> FloatCompareOperationFromCustom1(int16_t operation);
std::optional<LN_ThresholdOperation> ThresholdOperationFromCustom1(int16_t operation);
std::optional<LN_RangeOperation> RangeOperationFromCustom1(int16_t operation);

bool StringStartsWith(const std::string &value, const std::string &prefix);
bool StringEndsWith(const std::string &value, const std::string &suffix);
std::string ToCaseString(const std::string &value, bool upper);
std::string ZeroFillString(const std::string &value, int width);
int CountStringOccurrences(const std::string &value, const std::string &needle);
std::string ReplaceStringOccurrences(const std::string &value,
                                     const std::string &needle,
                                     const std::string &replacement);
std::string FormatStringSlots(const std::string &format,
                              const std::string &a,
                              const std::string &b,
                              const std::string &c,
                              const std::string &d);

std::string NormalizeInputEventName(std::string name);
std::string MouseButtonNameFromCustom(int custom2);
int32_t MouseInputStatusFromCustom(int custom1);
bool IsInputSocketLinked(const blender::bNode &node,
                         const std::string &socket_name,
                         const InputLinkMap &input_links);
SCA_IInputDevice::SCA_EnumInputs InputCodeFromName(const std::string &name);
int32_t GamepadButtonFromName(const std::string &name);
bool GamepadAddonIndexIsTrigger(int addon_index);
int32_t GamepadTriggerAxisFromAddonIndex(int addon_index);
int32_t GamepadButtonFromAddonIndex(int addon_index);

struct GamepadButtonTarget {
  bool is_trigger = false;
  int32_t index = 0;
};

GamepadButtonTarget GamepadButtonTargetFromNode(const blender::bNode &node,
                                                const NodeDefinitionMap &node_definitions,
                                                const InputLinkMap &input_links,
                                                ValueCache &value_cache);
uint32_t GamepadStickFromName(const std::string &name);

/* Per-kind expression output lowering (compile/LN_TreeCompiler_expression_outputs_by_kind.cc) */
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
                                            std::optional<uint32_t> &r_result);

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
                                      std::optional<uint32_t> &r_result);

/* BuildOutput* (implemented in compile/LN_TreeCompiler_output_expressions.cc) */
std::optional<uint32_t> BuildOutputBoolExpression(LN_Program &program,
                                                    const blender::bNode &node,
                                                    const blender::bNodeSocket &output_socket,
                                                    const NodeDefinitionMap &node_definitions,
                                                    const InputLinkMap &input_links,
                                                    ValueCache &value_cache,
                                                    FloatExpressionCache &float_expression_cache,
                                                    BoolExpressionCache &bool_expression_cache);

std::optional<uint32_t> BuildOutputIntExpression(
    LN_Program &program,
    const blender::bNode &node,
    const blender::bNodeSocket &output_socket,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    IntExpressionCache &int_expression_cache,
    BoolExpressionCache *bool_expression_cache = nullptr,
    FloatExpressionCache *float_expression_cache = nullptr,
    StringExpressionCache *string_expression_cache = nullptr,
    VectorExpressionCache *vector_expression_cache = nullptr,
    ColorExpressionCache *color_expression_cache = nullptr,
    ValueExpressionCache *value_expression_cache = nullptr);

std::optional<uint32_t> BuildOutputFloatExpression(
    LN_Program &program,
    const blender::bNode &node,
    const blender::bNodeSocket &output_socket,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    FloatExpressionCache &float_expression_cache,
    BoolExpressionCache *bool_expression_cache = nullptr,
    VectorExpressionCache *vector_expression_cache = nullptr);

std::optional<uint32_t> BuildOutputStringExpression(LN_Program &program,
                                                    const blender::bNode &node,
                                                    const blender::bNodeSocket &output_socket,
                                                    const NodeDefinitionMap &node_definitions,
                                                    const InputLinkMap &input_links,
                                                    ValueCache &value_cache,
                                                    StringExpressionCache &string_expression_cache);

std::optional<uint32_t> BuildOutputVectorExpression(LN_Program &program,
                                                     const blender::bNode &node,
                                                     const blender::bNodeSocket &output_socket,
                                                     const NodeDefinitionMap &node_definitions,
                                                     const InputLinkMap &input_links,
                                                     ValueCache &value_cache,
                                                     FloatExpressionCache &float_expression_cache,
                                                     VectorExpressionCache &vector_expression_cache);

std::optional<uint32_t> BuildOutputColorExpression(LN_Program &program,
                                                   const blender::bNode &node,
                                                   const blender::bNodeSocket &output_socket,
                                                   const NodeDefinitionMap &node_definitions,
                                                   const InputLinkMap &input_links,
                                                   ValueCache &value_cache,
                                                   FloatExpressionCache &float_expression_cache,
                                                   ColorExpressionCache &color_expression_cache);

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
    ValueExpressionCache &value_expression_cache);

/* BuildInput* (implemented in LN_TreeCompiler.cpp) */
std::optional<uint32_t> BuildInputBoolExpression(LN_Program &program,
                                                 const blender::bNode &node,
                                                 const std::string &socket_name,
                                                 const NodeDefinitionMap &node_definitions,
                                                 const InputLinkMap &input_links,
                                                 ValueCache &value_cache,
                                                 FloatExpressionCache &float_expression_cache,
                                                 BoolExpressionCache &bool_expression_cache);

std::optional<uint32_t> BuildInputExecutionExpression(LN_Program &program,
                                                      const blender::bNode &node,
                                                      const std::string &socket_name,
                                                      const NodeDefinitionMap &node_definitions,
                                                      const InputLinkMap &input_links,
                                                      ValueCache &value_cache,
                                                      FloatExpressionCache &float_expression_cache,
                                                      BoolExpressionCache &bool_expression_cache);

std::optional<uint32_t> BuildInputIntExpression(
    LN_Program &program,
    const blender::bNode &node,
    const std::string &socket_name,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    IntExpressionCache &int_expression_cache,
    BoolExpressionCache *bool_expression_cache = nullptr,
    FloatExpressionCache *float_expression_cache = nullptr,
    StringExpressionCache *string_expression_cache = nullptr,
    VectorExpressionCache *vector_expression_cache = nullptr,
    ColorExpressionCache *color_expression_cache = nullptr,
    ValueExpressionCache *value_expression_cache = nullptr);

std::optional<uint32_t> BuildInputFloatExpression(
    LN_Program &program,
    const blender::bNode &node,
    const std::string &socket_name,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    FloatExpressionCache &float_expression_cache,
    BoolExpressionCache *bool_expression_cache = nullptr,
    VectorExpressionCache *vector_expression_cache = nullptr);

std::optional<uint32_t> BuildInputStringExpression(LN_Program &program,
                                                   const blender::bNode &node,
                                                   const std::string &socket_name,
                                                   const NodeDefinitionMap &node_definitions,
                                                   const InputLinkMap &input_links,
                                                   ValueCache &value_cache,
                                                   StringExpressionCache &string_expression_cache);

std::optional<uint32_t> BuildInputVectorExpression(LN_Program &program,
                                                     const blender::bNode &node,
                                                     const std::string &socket_name,
                                                     const NodeDefinitionMap &node_definitions,
                                                     const InputLinkMap &input_links,
                                                     ValueCache &value_cache,
                                                     FloatExpressionCache &float_expression_cache,
                                                     VectorExpressionCache &vector_expression_cache);

std::optional<uint32_t> BuildInputRotationVectorExpression(
    LN_Program &program,
    const blender::bNode &node,
    const std::string &socket_name,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    FloatExpressionCache &float_expression_cache,
    VectorExpressionCache &vector_expression_cache);

std::optional<uint32_t> BuildInputColorExpression(LN_Program &program,
                                                   const blender::bNode &node,
                                                   const std::string &socket_name,
                                                   const NodeDefinitionMap &node_definitions,
                                                   const InputLinkMap &input_links,
                                                   ValueCache &value_cache,
                                                   FloatExpressionCache &float_expression_cache,
                                                   ColorExpressionCache &color_expression_cache);

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
    ValueExpressionCache &value_expression_cache);

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
    ValueExpressionCache &value_expression_cache);

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
    ValueExpressionCache &value_expression_cache);

std::optional<uint32_t> BuildMouseOverQueryExpression(LN_Program &program,
                                                      const blender::bNode &node,
                                                      const NodeDefinitionMap &node_definitions,
                                                      const InputLinkMap &input_links,
                                                      ValueCache &value_cache,
                                                      FloatExpressionCache &float_expression_cache,
                                                      BoolExpressionCache &bool_expression_cache);

std::optional<uint32_t> BuildRaycastQueryExpression(LN_Program &program,
                                                    const blender::bNode &node,
                                                    const NodeDefinitionMap &node_definitions,
                                                    const InputLinkMap &input_links,
                                                    ValueCache &value_cache,
                                                    FloatExpressionCache &float_expression_cache,
                                                    BoolExpressionCache &bool_expression_cache);
std::optional<uint32_t> BuildRaycastAllQueryExpression(
    LN_Program &program,
    const blender::bNode &node,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    FloatExpressionCache &float_expression_cache,
    BoolExpressionCache &bool_expression_cache);
std::optional<uint32_t> BuildShapeCastQueryExpression(
    LN_Program &program,
    const blender::bNode &node,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    FloatExpressionCache &float_expression_cache,
    BoolExpressionCache &bool_expression_cache);
std::optional<uint32_t> BuildShapeCastAllQueryExpression(
    LN_Program &program,
    const blender::bNode &node,
    const NodeDefinitionMap &node_definitions,
    const InputLinkMap &input_links,
    ValueCache &value_cache,
    FloatExpressionCache &float_expression_cache,
    BoolExpressionCache &bool_expression_cache);

std::optional<uint32_t> BuildMouseRayQueryExpression(LN_Program &program,
                                                     const blender::bNode &node,
                                                     const NodeDefinitionMap &node_definitions,
                                                     const InputLinkMap &input_links,
                                                     ValueCache &value_cache,
                                                     FloatExpressionCache &float_expression_cache,
                                                     BoolExpressionCache &bool_expression_cache);

std::optional<uint32_t> BuildCameraRayQueryExpression(LN_Program &program,
                                                      const blender::bNode &node,
                                                      const NodeDefinitionMap &node_definitions,
                                                      const InputLinkMap &input_links,
                                                      ValueCache &value_cache,
                                                      FloatExpressionCache &float_expression_cache,
                                                      BoolExpressionCache &bool_expression_cache);

}  // namespace ln_compiler
