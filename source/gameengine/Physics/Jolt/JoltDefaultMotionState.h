/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Contributor(s): UPBGE Contributors
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file JoltDefaultMotionState.h
 *  \ingroup physjolt
 *  \brief Simple PHY_IMotionState that stores position/orientation directly.
 *
 * Used for sensor controllers (sphere/cone) that are not attached to a scene graph node.
 * Game objects use KX_MotionState instead (passed in via ConvertObject).
 */

#pragma once

#include "PHY_IMotionState.h"

class JoltDefaultMotionState : public PHY_IMotionState {
 public:
  JoltDefaultMotionState();
  virtual ~JoltDefaultMotionState();

  virtual MT_Vector3 GetWorldPosition() const override;
  virtual MT_Vector3 GetWorldScaling() const override;
  virtual MT_Matrix3x3 GetWorldOrientation() const override;

  virtual void SetWorldPosition(const MT_Vector3 &pos) override;
  virtual void SetWorldOrientation(const MT_Matrix3x3 &ori) override;
  virtual void SetWorldOrientation(const MT_Quaternion &quat) override;

  virtual void CalculateWorldTransformations() override;

 private:
  MT_Vector3 m_worldPosition;
  MT_Vector3 m_worldScaling;
  MT_Matrix3x3 m_worldOrientation;
};
