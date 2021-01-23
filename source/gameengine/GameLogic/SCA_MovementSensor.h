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
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file SCA_MovementSensor.h
 *  \ingroup gamelogic
 *  \brief Check if object has moved
 */

#pragma once

#include "MT_Vector3.h"
#include "SCA_ISensor.h"

class SCA_MovementSensor : public SCA_ISensor {
  Py_Header

      public : enum MovementAxis {
        KX_MOVEMENT_AXIS_POS_X = 1,
        KX_MOVEMENT_AXIS_POS_Y = 0,
        KX_MOVEMENT_AXIS_POS_Z = 2,
        KX_MOVEMENT_AXIS_NEG_X = 3,
        KX_MOVEMENT_AXIS_NEG_Y = 4,
        KX_MOVEMENT_AXIS_NEG_Z = 5,
        KX_MOVEMENT_ALL_AXIS = 6,
      };

 private:
  /// True if the position is taken in world space or object space(local).
  bool m_localflag;
  /// The axis to detect mouvement, can be all axis.
  int m_axis;
  /// The previous object position.
  MT_Vector3 m_previousPosition;
  /** True if the position is not the same (depends of a treshold value)
   * between two logic frame.
   */
  bool m_positionHasChanged;
  /// Threshold below which the movement is not detected
  float m_threshold;
  bool m_triggered;

 public:
  SCA_MovementSensor(
      SCA_EventManager *eventmgr, SCA_IObject *gameobj, int axis, bool localflag, float threshold);
  virtual ~SCA_MovementSensor();
  virtual EXP_Value *GetReplica();
  MT_Vector3 GetOwnerPosition(bool local);

  virtual bool Evaluate();
  virtual bool IsPositiveTrigger();
  virtual void Init();

#ifdef WITH_PYTHON

#endif  // WITH_PYTHON
};
