/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_QueryDiagnostics.cpp
 *  \ingroup logicnodes
 */

#include "LN_QueryDiagnostics.h"

#include <sstream>

namespace {

std::string vector_to_string(const MT_Vector3 &value)
{
  std::ostringstream stream;
  stream << '(' << value.x() << ", " << value.y() << ", " << value.z() << ')';
  return stream.str();
}

}  // namespace

const char *LN_QueryDiagnosticStatusName(const LN_QueryDiagnosticStatus status)
{
  switch (status) {
    case LN_QueryDiagnosticStatus::NotEvaluated:
      return "NotEvaluated";
    case LN_QueryDiagnosticStatus::Hit:
      return "Hit";
    case LN_QueryDiagnosticStatus::NoHit:
      return "NoHit";
    case LN_QueryDiagnosticStatus::Disabled:
      return "Disabled";
    case LN_QueryDiagnosticStatus::InvalidTarget:
      return "InvalidTarget";
    case LN_QueryDiagnosticStatus::InvalidFilter:
      return "InvalidFilter";
    case LN_QueryDiagnosticStatus::MissingPhysicsWorld:
      return "MissingPhysicsWorld";
    case LN_QueryDiagnosticStatus::UnsupportedPhysicsBackend:
      return "UnsupportedPhysicsBackend";
    case LN_QueryDiagnosticStatus::UnavailableUnsnapshottedData:
      return "UnavailableUnsnapshottedData";
  }
  return "Unknown";
}

std::string LN_DescribePhysicsQueryResult(const LN_PhysicsQueryResult &result)
{
  std::ostringstream stream;
  stream << "status=" << LN_QueryDiagnosticStatusName(result.diagnostic_status);
  stream << " hit=" << (result.hit ? "true" : "false");
  stream << " point=" << vector_to_string(result.hit_position);
  stream << " normal=" << vector_to_string(result.hit_normal);
  stream << " direction=" << vector_to_string(result.ray_direction);
  stream << " fraction=" << result.hit_fraction;
  stream << " distance=" << result.hit_distance;
  stream << " has_uv=" << (result.has_uv ? "true" : "false");
  if (result.has_uv) {
    stream << " uv=(" << result.hit_uv.x() << "," << result.hit_uv.y() << ")";
  }
  if (result.polygon_index >= 0) {
    stream << " polygon=" << result.polygon_index;
  }
  return stream.str();
}
