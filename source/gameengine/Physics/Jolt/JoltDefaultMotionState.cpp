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

/** \file JoltDefaultMotionState.cpp
 *  \ingroup physjolt
 */

#include "JoltDefaultMotionState.h"

JoltDefaultMotionState::JoltDefaultMotionState()
    : m_worldPosition(0.0f, 0.0f, 0.0f),
      m_worldScaling(1.0f, 1.0f, 1.0f),
      m_worldOrientation(MT_Matrix3x3::Identity())
{
}

JoltDefaultMotionState::~JoltDefaultMotionState()
{
}

MT_Vector3 JoltDefaultMotionState::GetWorldPosition() const
{
  return m_worldPosition;
}

MT_Vector3 JoltDefaultMotionState::GetWorldScaling() const
{
  return m_worldScaling;
}

MT_Matrix3x3 JoltDefaultMotionState::GetWorldOrientation() const
{
  return m_worldOrientation;
}

void JoltDefaultMotionState::SetWorldPosition(const MT_Vector3 &pos)
{
  m_worldPosition = pos;
}

void JoltDefaultMotionState::SetWorldOrientation(const MT_Matrix3x3 &ori)
{
  m_worldOrientation = ori;
}

void JoltDefaultMotionState::SetWorldOrientation(const MT_Quaternion &quat)
{
  m_worldOrientation = MT_Matrix3x3(quat);
}

void JoltDefaultMotionState::CalculateWorldTransformations()
{
  /* Nothing to do — transforms are set directly. */
}
