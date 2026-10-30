/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_ValueInterpolation.h
 *  \ingroup logicnodes
 *
 * Stateless interpolation for generic Logic Nodes values.
 */

#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

#include "LN_Types.h"

namespace ln::value_interpolation {

inline float LerpFloat(const float a, const float b, const float factor)
{
  if (factor == 0.0f) {
    return a;
  }
  if (factor == 1.0f) {
    return b;
  }
  /* Double intermediates avoid overflow in (b - a) for large, otherwise representable floats. */
  return float(double(a) + (double(b) - double(a)) * double(factor));
}

inline int32_t LerpInt(const int32_t a, const int32_t b, const float factor)
{
  const double mixed = double(a) + (double(b) - double(a)) * double(factor);
  const double rounded = std::round(mixed);
  constexpr double min_int = double(std::numeric_limits<int32_t>::min());
  constexpr double max_int = double(std::numeric_limits<int32_t>::max());
  if (rounded <= min_int) {
    return std::numeric_limits<int32_t>::min();
  }
  if (rounded >= max_int) {
    return std::numeric_limits<int32_t>::max();
  }
  return int32_t(rounded);
}

inline LN_Value Mix(const LN_Value &a, const LN_Value &b, float factor)
{
  if (!a.exists || !b.exists) {
    return {};
  }
  if (!std::isfinite(factor)) {
    factor = 0.0f;
  }

  LN_Value result;
  result.exists = true;

  /* Numeric promotion makes an Integer/Float pair useful without lossy implicit integer output. */
  if ((a.type == LN_ValueType::Int || a.type == LN_ValueType::Float) &&
      (b.type == LN_ValueType::Int || b.type == LN_ValueType::Float) && a.type != b.type)
  {
    const float a_value = a.type == LN_ValueType::Int ? float(a.int_value) : a.float_value;
    const float b_value = b.type == LN_ValueType::Int ? float(b.int_value) : b.float_value;
    result.type = LN_ValueType::Float;
    result.float_value = LerpFloat(a_value, b_value, factor);
    return result;
  }

  if (a.type != b.type) {
    return {};
  }

  result.type = a.type;
  switch (a.type) {
    case LN_ValueType::Int:
      result.int_value = LerpInt(a.int_value, b.int_value, factor);
      return result;
    case LN_ValueType::Float:
      result.float_value = LerpFloat(a.float_value, b.float_value, factor);
      return result;
    case LN_ValueType::Vector:
      result.vector_value = MT_Vector3(LerpFloat(a.vector_value.x(), b.vector_value.x(), factor),
                                       LerpFloat(a.vector_value.y(), b.vector_value.y(), factor),
                                       LerpFloat(a.vector_value.z(), b.vector_value.z(), factor));
      return result;
    case LN_ValueType::Vector4:
      result.vector4_value = MT_Vector4(
          LerpFloat(a.vector4_value.x(), b.vector4_value.x(), factor),
          LerpFloat(a.vector4_value.y(), b.vector4_value.y(), factor),
          LerpFloat(a.vector4_value.z(), b.vector4_value.z(), factor),
          LerpFloat(a.vector4_value.w(), b.vector4_value.w(), factor));
      return result;
    case LN_ValueType::Color:
      result.color_value = MT_Vector4(LerpFloat(a.color_value.x(), b.color_value.x(), factor),
                                      LerpFloat(a.color_value.y(), b.color_value.y(), factor),
                                      LerpFloat(a.color_value.z(), b.color_value.z(), factor),
                                      LerpFloat(a.color_value.w(), b.color_value.w(), factor));
      return result;
    case LN_ValueType::Rotation: {
      /* Preserve authored Euler winding at exact endpoints. The quaternion is authoritative for
       * interpolation, but generic graph boundaries intentionally retain this compatibility hint.
       */
      if (factor == 0.0f || factor == 1.0f) {
        const LN_Value &endpoint = factor == 0.0f ? a : b;
        result.rotation_value = endpoint.rotation_value;
        result.vector_value = endpoint.vector_value;
        result.rotation_euler_hint_valid = endpoint.rotation_euler_hint_valid;
        return result;
      }
      const LN_Rotation rotation = a.rotation_value.Slerp(b.rotation_value, factor);
      result.SetRotation(rotation);
      return result;
    }
    case LN_ValueType::None:
    case LN_ValueType::Bool:
    case LN_ValueType::Matrix:
    case LN_ValueType::String:
    case LN_ValueType::ObjectRef:
    case LN_ValueType::SceneRef:
    case LN_ValueType::CollectionRef:
    case LN_ValueType::DatablockRef:
    case LN_ValueType::List:
    case LN_ValueType::Dict:
    case LN_ValueType::Generic:
      return {};
  }
  return {};
}

}  // namespace ln::value_interpolation
