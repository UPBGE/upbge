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

/** \file SCA_NearSensor.h
 *  \ingroup ketsji
 *  \brief Sense if other objects are near
 */

#pragma once

#include "KX_ClientObjectInfo.h"
#include "SCA_CollisionSensor.h"

class KX_Scene;
class PHY_ICollData;

class SCA_NearSensor : public SCA_CollisionSensor {
  Py_Header protected : float m_Margin;
  float m_ResetMargin;

  KX_ClientObjectInfo *m_client_info;

 public:
  SCA_NearSensor(class SCA_EventManager *eventmgr,
                 class KX_GameObject *gameobj,
                 float margin,
                 float resetmargin,
                 bool bFindMaterial,
                 const std::string &touchedpropname,
                 PHY_IPhysicsController *ctrl);
#if 0
public:
	SCA_NearSensor(class SCA_EventManager* eventmgr,
			class KX_GameObject* gameobj,
			double margin,
			double resetmargin,
			bool bFindMaterial,
			const std::string& touchedpropname,
			class KX_Scene* scene);
#endif
  virtual ~SCA_NearSensor();
  virtual void SynchronizeTransform();
  virtual EXP_Value *GetReplica();
  virtual void ProcessReplica();
  virtual void SetPhysCtrlRadius();
  virtual bool Evaluate();

  virtual void ReParent(SCA_IObject *parent);
  virtual bool NewHandleCollision(PHY_IPhysicsController *ctrl1,
                                  PHY_IPhysicsController *ctrl2,
                                  const PHY_ICollData *coll_data);
  virtual bool BroadPhaseFilterCollision(PHY_IPhysicsController *ctrl1, PHY_IPhysicsController *ctrl2);
  virtual bool BroadPhaseSensorFilterCollision(PHY_IPhysicsController *ctrl1, PHY_IPhysicsController *ctrl2)
  {
    return false;
  }
  virtual sensortype GetSensorType()
  {
    return ST_NEAR;
  }

#ifdef WITH_PYTHON

  /* --------------------------------------------------------------------- */
  /* Python interface ---------------------------------------------------- */
  /* --------------------------------------------------------------------- */

  // No methods

  // This method is used to make sure the distance does not exceed the reset distance
  static int CheckResetDistance(EXP_PyObjectPlus *self, const PyAttributeDef *)
  {
    SCA_NearSensor *sensor = reinterpret_cast<SCA_NearSensor *>(self);

    if (sensor->m_Margin > sensor->m_ResetMargin)
      sensor->m_ResetMargin = sensor->m_Margin;

    sensor->SetPhysCtrlRadius();

    return 0;
  }

#endif /* WITH_PYTHON */
};
