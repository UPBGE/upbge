/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2017 Tristan Porteries */

/** \file SG_Frustum.h
 *  \ingroup bgesg
 */

#pragma once

#include <array>

#include "MT_Matrix4x4.h"

/// \brief Camera frustum data.
class SG_Frustum {
 private:
  /// Camera modelview multiplied by projection.
  MT_Matrix4x4 m_matrix;
  /// Frustum planes.
  std::array<MT_Vector4, 6> m_planes;

 public:
  enum TestType { INSIDE, INTERSECT, OUTSIDE };

  SG_Frustum() = default;
  SG_Frustum(const MT_Matrix4x4 &matrix);
  ~SG_Frustum() = default;

  const std::array<MT_Vector4, 6> &GetPlanes() const;
  const MT_Matrix4x4 &GetMatrix() const;

  TestType PointInsideFrustum(const MT_Vector3 &point) const;
  TestType SphereInsideFrustum(const MT_Vector3 &center, float radius) const;
  TestType BoxInsideFrustum(const std::array<MT_Vector3, 8> &box) const;
  TestType AabbInsideFrustum(const MT_Vector3 &min,
                             const MT_Vector3 &max,
                             const MT_Matrix4x4 &mat) const;
  TestType FrustumInsideFrustum(const SG_Frustum &frustum) const;
};
