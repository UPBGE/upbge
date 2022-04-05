/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved. */

/** \file SG_BBox.h
 *  \ingroup bgesg
 *  \brief Bounding Box
 */

#pragma once

#include "MT_Vector3.h"

/**
 * Bounding box class.
 * Holds the minimum and maximum axis aligned points of a node's bounding box,
 * in world coordinates.
 */
class SG_BBox {
 private:
  /// AABB data.
  MT_Vector3 m_min;
  MT_Vector3 m_max;

  /// Sphere data.
  MT_Vector3 m_center;
  float m_radius;

  /// Update sphere data with current AABB data.
  void UpdateSphere();

 public:
  SG_BBox();
  SG_BBox(const MT_Vector3 &min, const MT_Vector3 &max);
  ~SG_BBox() = default;

  const MT_Vector3 &GetCenter() const;
  const float GetRadius() const;

  const MT_Vector3 &GetMin() const;
  const MT_Vector3 &GetMax() const;

  void Get(MT_Vector3 &min, MT_Vector3 &max) const;

  void SetMin(const MT_Vector3 &min);
  void SetMax(const MT_Vector3 &max);

  void Set(const MT_Vector3 &min, const MT_Vector3 &max);

  /// Test if the given point is inside this bounding box.
  bool Inside(const MT_Vector3 &point) const;
};
