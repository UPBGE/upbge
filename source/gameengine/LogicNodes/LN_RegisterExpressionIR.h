/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_RegisterExpressionIR.h
 *  \ingroup logicnodes
 */

#pragma once

#include <cstdint>
#include <vector>

#include "LN_Performance.h"
#include "LN_Types.h"

static constexpr uint32_t LN_REGISTER_EXPRESSION_IR_CACHE_GENERATION = 48;
static constexpr uint32_t LN_REGISTER_EXPRESSION_IR_FEATURE_MASK =
    uint32_t(LN_RUNTIME_FEATURE_REGISTER_EXPRESSION_EVALUATOR);

enum class LN_RegisterValueKind : uint8_t {
  Bool = 0,
  Int,
  Float,
  Vector,
  Color,
  Object,
  String,
  StringId,
  GenericValue,
};

enum class LN_RegisterExpressionOpKind : uint8_t {
  BoolConstant = 0,
  BoolSnapshotRead,
  BoolRuntimePropertyRead,
  BoolInputRead,
  BoolNot,
  BoolAnd,
  BoolOr,
  BoolFloatCompare,
  BoolStringPredicate,
  BoolValueIsNone,
  BoolFromValue,
  BoolValueCompare,
  IntConstant,
  IntSnapshotRead,
  IntRuntimePropertyRead,
  IntInputRead,
  IntFromValue,
  IntRandom,
  IntDictLength,
  IntListLength,
  IntStringCount,
  FloatConstant,
  FloatSnapshotRead,
  FloatRuntimePropertyRead,
  FloatInputRead,
  FloatFromValue,
  FloatAdd,
  FloatSubtract,
  FloatMultiply,
  FloatDivide,
  FloatPower,
  FloatMinimum,
  FloatMaximum,
  FloatAbsolute,
  FloatSign,
  FloatRound,
  FloatFloor,
  FloatCeil,
  FloatTruncate,
  FloatFraction,
  FloatModulo,
  FloatSine,
  FloatCosine,
  FloatRadians,
  FloatDegrees,
  FloatNegate,
  FloatClamp,
  FloatThreshold,
  FloatRangedThreshold,
  FloatSelect,
  FloatVectorComponent,
  FloatColorComponent,
  FloatRandom,
  FloatFormula,
  VectorConstant,
  VectorSnapshotRead,
  VectorRuntimePropertyRead,
  VectorInputRead,
  VectorFromValue,
  VectorAdd,
  VectorSubtract,
  VectorMultiply,
  VectorDivide,
  VectorAbsolute,
  VectorMinimum,
  VectorMaximum,
  VectorScale,
  VectorNormalize,
  VectorResize,
  VectorRotateAroundAxis,
  VectorToRotation,
  VectorCombine,
  VectorRandom,
  ColorConstant,
  ColorSnapshotRead,
  ColorRuntimePropertyRead,
  ColorCombine,
  ColorFromValue,
  StringIdConstant,
  StringSnapshotRead,
  StringRuntimePropertyRead,
  StringJoin,
  StringReplace,
  StringToUppercase,
  StringToLowercase,
  StringZeroFill,
  StringFormat,
  StringFromValue,
  ValueConstant,
  ValueSnapshotRead,
  ValueRuntimePropertyRead,
  ValueSelect,
  ValueFromBool,
  ValueFromInt,
  ValueFromFloat,
  ValueFromString,
  ValueFromVector,
  ValueFromColor,
  ValueFromRotation,
  ValueCombineVector4,
  ValueResizeVector,
  ValueEulerToMatrix,
  ValueMatrixToEuler,
  ValueMakeList,
  ValueListFromItems,
  ValueListDuplicate,
  ValueListExtend,
  ValueListAppend,
  ValueListRemoveIndex,
  ValueListRemoveValue,
  ValueListSetIndex,
  ValueListElement,
  ValueListRandomItem,
  ValueSwitchList,
  ValueSwitchListCompare,
  ValueEmptyList,
  ValueEmptyDict,
  ValueMakeDict,
  ValueDictGetKey,
  ValueDictSetKey,
  ValueDictRemoveKey,
  ValueDictRemoveKeyValue,
  ValueDictMerge,
  ValueDictGetKeys,
  Fallback,
};

struct LN_RegisterExpressionRef {
  LN_RegisterValueKind value_kind = LN_RegisterValueKind::GenericValue;
  uint32_t expression_index = LN_INVALID_INDEX;
  uint32_t register_index = LN_INVALID_INDEX;
};

struct LN_RegisterExpressionOp {
  LN_RegisterExpressionOpKind kind = LN_RegisterExpressionOpKind::Fallback;
  LN_RegisterValueKind output_kind = LN_RegisterValueKind::GenericValue;
  uint32_t expression_index = LN_INVALID_INDEX;
  uint32_t output_register = LN_INVALID_INDEX;
  LN_RegisterExpressionRef input0;
  LN_RegisterExpressionRef input1;
  LN_RegisterExpressionRef input2;
  LN_RegisterExpressionRef input3;
  uint32_t variable_ref_start = 0;
  uint32_t variable_ref_count = 0;
  uint8_t component_index = 0;
  LN_FloatCompareOperation float_compare_operation = LN_FloatCompareOperation::Equal;
  LN_ThresholdOperation threshold_operation = LN_ThresholdOperation::Greater;
  LN_RangeOperation range_operation = LN_RangeOperation::Inside;
};

struct LN_RegisterExpressionLifetime {
  LN_RegisterValueKind value_kind = LN_RegisterValueKind::GenericValue;
  uint32_t expression_index = LN_INVALID_INDEX;
  uint32_t register_index = LN_INVALID_INDEX;
  uint32_t first_op_index = LN_INVALID_INDEX;
  uint32_t last_use_op_index = LN_INVALID_INDEX;
};

struct LN_RegisterExpressionSoABatch {
  LN_RegisterExpressionOpKind kind = LN_RegisterExpressionOpKind::Fallback;
  uint32_t first_op_index = 0;
  uint32_t op_count = 0;
  bool scalar_fallback_available = true;
  bool simd_candidate = false;
};

struct LN_RegisterExpressionProgram {
  uint32_t program_version = LN_PROGRAM_VERSION;
  uint32_t schema_version = LN_PROGRAM_SCHEMA_VERSION;
  uint32_t runtime_build_flags = 0;
  uint32_t cache_generation = LN_REGISTER_EXPRESSION_IR_CACHE_GENERATION;
  uint32_t feature_mask = LN_REGISTER_EXPRESSION_IR_FEATURE_MASK;

  std::vector<LN_RegisterExpressionOp> ops;
  std::vector<LN_RegisterExpressionRef> variable_refs;
  std::vector<LN_RegisterExpressionLifetime> lifetimes;
  std::vector<LN_RegisterExpressionSoABatch> soa_batches;

  std::vector<uint32_t> bool_expression_registers;
  std::vector<uint32_t> int_expression_registers;
  std::vector<uint32_t> float_expression_registers;
  std::vector<uint32_t> vector_expression_registers;
  std::vector<uint32_t> color_expression_registers;
  std::vector<uint32_t> string_expression_registers;
  std::vector<LN_RegisterValueKind> string_expression_register_kinds;
  std::vector<uint32_t> value_expression_registers;

  uint32_t bool_register_count = 0;
  uint32_t int_register_count = 0;
  uint32_t float_register_count = 0;
  uint32_t vector_register_count = 0;
  uint32_t color_register_count = 0;
  uint32_t object_register_count = 0;
  uint32_t string_register_count = 0;
  uint32_t string_id_register_count = 0;
  uint32_t generic_value_register_count = 0;

  uint32_t fallback_expression_count = 0;
  uint32_t scalar_op_count = 0;
  uint32_t simd_candidate_batch_count = 0;
  uint32_t simd_candidate_lane_count = 0;
  bool scalar_fallback_available = true;
  bool valid = true;
};
