/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_TypeConversion.h
 *  \ingroup logicnodes
 */

#pragma once

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <string>

#include "LN_Types.h"

inline LN_FloatToIntMode LN_ValidFloatToIntMode(const LN_FloatToIntMode mode)
{
  switch (mode) {
    case LN_FloatToIntMode::Truncate:
    case LN_FloatToIntMode::Round:
    case LN_FloatToIntMode::Floor:
    case LN_FloatToIntMode::Ceil:
      return mode;
  }
  return LN_FloatToIntMode::Truncate;
}

/**
 * Convert without invoking C++'s undefined behavior for NaN or out-of-range floating-point
 * values. NaN maps to zero and finite/infinite values outside int32 range saturate.
 */
inline int32_t LN_ConvertFloatToInt(const float value, const LN_FloatToIntMode requested_mode)
{
  if (std::isnan(value)) {
    return 0;
  }

  double converted = double(value);
  switch (LN_ValidFloatToIntMode(requested_mode)) {
    case LN_FloatToIntMode::Truncate:
      converted = std::trunc(converted);
      break;
    case LN_FloatToIntMode::Round:
      converted = std::round(converted);
      break;
    case LN_FloatToIntMode::Floor:
      converted = std::floor(converted);
      break;
    case LN_FloatToIntMode::Ceil:
      converted = std::ceil(converted);
      break;
  }

  constexpr double min_int = double(std::numeric_limits<int32_t>::min());
  constexpr double max_int = double(std::numeric_limits<int32_t>::max());
  if (converted <= min_int) {
    return std::numeric_limits<int32_t>::min();
  }
  if (converted >= max_int) {
    return std::numeric_limits<int32_t>::max();
  }
  return int32_t(converted);
}

/**
 * Preserve the established Typecast Value parsing behavior: a numeric prefix is accepted.
 */
inline bool LN_TryParseTypecastFloat(const std::string &text_value, float &r_value)
{
  const char *text = text_value.c_str();
  char *end = nullptr;
  const float parsed = std::strtof(text, &end);
  if (end == text) {
    return false;
  }
  r_value = parsed;
  return true;
}

inline std::string LN_TypecastFloatToString(const float value)
{
  std::ostringstream stream;
  stream << value;
  return stream.str();
}

inline std::string LN_TypecastVectorToString(const MT_Vector3 &value)
{
  return "(" + LN_TypecastFloatToString(value.x()) + ", " + LN_TypecastFloatToString(value.y()) +
         ", " + LN_TypecastFloatToString(value.z()) + ")";
}

inline std::string LN_TypecastColorToString(const MT_Vector4 &value)
{
  return "(" + LN_TypecastFloatToString(value.x()) + ", " + LN_TypecastFloatToString(value.y()) +
         ", " + LN_TypecastFloatToString(value.z()) + ", " + LN_TypecastFloatToString(value.w()) +
         ")";
}
