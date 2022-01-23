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

/** \file KX_CollisionEventManager.h
 *  \ingroup ketsji
 */

#pragma once

#include <set>
#include <vector>

#include "KX_GameObject.h"
#include "SCA_CollisionSensor.h"
#include "SCA_EventManager.h"

class SCA_ISensor;
class PHY_IPhysicsEnvironment;

class KX_CollisionEventManager : public SCA_EventManager {
  /**
   * Contains two colliding objects and the first contact point.
   */
  class NewCollision {
   public:
    PHY_IPhysicsController *first;
    PHY_IPhysicsController *second;
    const PHY_ICollData *colldata;
    bool isFirst;

    /**
     * Creates a copy of the given PHY_ICollData; freeing that copy should be done by the owner of
     * the NewCollision object.
     *
     * This allows us to efficiently store NewCollision objects in a std::set without creating more
     * copies of colldata, as the NewCollision copy constructor reuses the pointer and doesn't
     * clone it again. */
    NewCollision(PHY_IPhysicsController *first,
                 PHY_IPhysicsController *second,
                 const PHY_ICollData *colldata,
                 bool isFirst);
    NewCollision(const NewCollision &to_copy);
    bool operator<(const NewCollision &other) const;
  };

  PHY_IPhysicsEnvironment *m_physEnv;

  std::set<NewCollision> m_newCollisions;

  static bool newCollisionResponse(void *client_data,
                                   PHY_IPhysicsController *ctrl1,
                                   PHY_IPhysicsController *ctrl2,
                                   const PHY_ICollData *coll_data,
                                   bool first);

  static bool newBroadphaseResponse(void *client_data,
                                    PHY_IPhysicsController *ctrl1,
                                    PHY_IPhysicsController *ctrl2,
                                    const PHY_ICollData *coll_data,
                                    bool first);

  virtual bool NewHandleCollision(PHY_IPhysicsController *ctrl1,
                                  PHY_IPhysicsController *ctrl2,
                                  const PHY_ICollData *coll_data,
                                  bool first);

  void RemoveNewCollisions();

 public:
  KX_CollisionEventManager(class SCA_LogicManager *logicmgr, PHY_IPhysicsEnvironment *physEnv);
  virtual ~KX_CollisionEventManager();

  virtual void NextFrame();
  virtual void EndFrame();
  virtual bool RegisterSensor(SCA_ISensor *sensor);
  virtual bool RemoveSensor(SCA_ISensor *sensor);

  SCA_LogicManager *GetLogicManager();
  PHY_IPhysicsEnvironment *GetPhysicsEnvironment();
};
