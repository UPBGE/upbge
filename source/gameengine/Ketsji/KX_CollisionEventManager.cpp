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

/** \file gameengine/Ketsji/KX_CollisionEventManager.cpp
 *  \ingroup ketsji
 */

#include "KX_CollisionEventManager.h"

#include "KX_CollisionContactPoints.h"
#include "PHY_IPhysicsController.h"
#include "PHY_IPhysicsEnvironment.h"

KX_CollisionEventManager::KX_CollisionEventManager(class SCA_LogicManager *logicmgr,
                                                   PHY_IPhysicsEnvironment *physEnv)
    : SCA_EventManager(logicmgr, TOUCH_EVENTMGR), m_physEnv(physEnv)
{
  m_physEnv->AddCollisionCallback(
      PHY_OBJECT_RESPONSE, KX_CollisionEventManager::newCollisionResponse, this);
  m_physEnv->AddCollisionCallback(
      PHY_SENSOR_RESPONSE, KX_CollisionEventManager::newCollisionResponse, this);
  m_physEnv->AddCollisionCallback(
      PHY_BROADPH_RESPONSE, KX_CollisionEventManager::newBroadphaseResponse, this);
}

KX_CollisionEventManager::~KX_CollisionEventManager()
{
  RemoveNewCollisions();
}

void KX_CollisionEventManager::RemoveNewCollisions()
{
  m_newCollisions.clear();
}

bool KX_CollisionEventManager::NewHandleCollision(PHY_IPhysicsController *ctrl1,
                                                  PHY_IPhysicsController *ctrl2,
                                                  const PHY_ICollData *coll_data,
                                                  bool first)
{
  m_newCollisions.insert(NewCollision(ctrl1, ctrl2, coll_data, first));

  return false;
}

bool KX_CollisionEventManager::newCollisionResponse(void *client_data,
                                                    PHY_IPhysicsController *ctrl1,
                                                    PHY_IPhysicsController *ctrl2,
                                                    const PHY_ICollData *coll_data,
                                                    bool first)
{
  KX_CollisionEventManager *collisionmgr = (KX_CollisionEventManager *)client_data;
  collisionmgr->NewHandleCollision(ctrl1, ctrl2, coll_data, first);
  return false;
}

bool KX_CollisionEventManager::newBroadphaseResponse(void *client_data,
                                                     PHY_IPhysicsController *ctrl1,
                                                     PHY_IPhysicsController *ctrl2,
                                                     const PHY_ICollData *coll_data,
                                                     bool first)
{
  KX_ClientObjectInfo *info1 = (ctrl1) ?
                                   static_cast<KX_ClientObjectInfo *>(ctrl1->GetNewClientInfo()) :
                                   nullptr;
  KX_ClientObjectInfo *info2 = (ctrl2) ?
                                   static_cast<KX_ClientObjectInfo *>(ctrl2->GetNewClientInfo()) :
                                   nullptr;

  // This call back should only be called for controllers of Near and Radar sensor
  if (!info1) {
    return true;
  }
  bool has_py_callbacks = false;

#ifdef WITH_PYTHON
  // Get KX_GameObjects for callbacks
  KX_GameObject *gobj1 = info1->m_gameobject;
  KX_GameObject *gobj2 = (info2) ? info2->m_gameobject : nullptr;

  // Consider callbacks for broadphase inclusion if it's a sensor object type
  if (gobj1 && gobj2) {
    has_py_callbacks = gobj1->m_collisionCallbacks || gobj2->m_collisionCallbacks;
  }
#endif

  switch (info1->m_type) {
    case KX_ClientObjectInfo::SENSOR:
      if (info1->m_sensors.size() == 1) {
        // only one sensor for this type of object
        SCA_CollisionSensor *collisionsensor = static_cast<SCA_CollisionSensor *>(
            *info1->m_sensors.begin());
        return collisionsensor->BroadPhaseFilterCollision(ctrl1, ctrl2);
      }
      break;
    case KX_ClientObjectInfo::OBSENSOR:
    case KX_ClientObjectInfo::OBACTORSENSOR:
      // this object may have multiple collision sensors,
      // check is any of them is interested in this object
      for (SCA_ISensor *sensor : info1->m_sensors) {
        if (sensor->GetSensorType() == SCA_ISensor::ST_TOUCH) {
          SCA_CollisionSensor *collisionsensor = static_cast<SCA_CollisionSensor *>(sensor);
          if (collisionsensor->BroadPhaseSensorFilterCollision(ctrl1, ctrl2)) {
            return true;
          }
        }
      }

      return has_py_callbacks;

    // quiet the compiler
    case KX_ClientObjectInfo::STATIC:
    case KX_ClientObjectInfo::ACTOR:
    case KX_ClientObjectInfo::RESERVED1:
      /* do nothing*/
      break;
  }
  return true;
}

bool KX_CollisionEventManager::RegisterSensor(SCA_ISensor *sensor)
{
  if (SCA_EventManager::RegisterSensor(sensor)) {
    SCA_CollisionSensor *collisionsensor = static_cast<SCA_CollisionSensor *>(sensor);
    // the sensor was effectively inserted, register it
    collisionsensor->RegisterSumo(this);
    return true;
  }

  return false;
}

bool KX_CollisionEventManager::RemoveSensor(SCA_ISensor *sensor)
{
  if (SCA_EventManager::RemoveSensor(sensor)) {
    SCA_CollisionSensor *collisionsensor = static_cast<SCA_CollisionSensor *>(sensor);
    // the sensor was effectively removed, unregister it
    collisionsensor->UnregisterSumo(this);
    return true;
  }

  return false;
}

void KX_CollisionEventManager::EndFrame()
{
  for (SCA_ISensor *sensor : m_sensors) {
    static_cast<SCA_CollisionSensor *>(sensor)->EndFrame();
  }
}

void KX_CollisionEventManager::NextFrame()
{
  for (SCA_ISensor *sensor : m_sensors) {
    static_cast<SCA_CollisionSensor *>(sensor)->SynchronizeTransform();
  }

  for (const NewCollision &collision : m_newCollisions) {
    // Controllers
    PHY_IPhysicsController *ctrl1 = collision.first;
    PHY_IPhysicsController *ctrl2 = collision.second;
    // Sensor iterator
    std::list<SCA_ISensor *>::iterator sit;

    // First client info
    KX_ClientObjectInfo *client_info = static_cast<KX_ClientObjectInfo *>(
        ctrl1->GetNewClientInfo());
    // First gameobject
    KX_GameObject *kxObj1 = KX_GameObject::GetClientObject(client_info);
    // Invoke sensor response for each object
    if (client_info) {
      for (sit = client_info->m_sensors.begin(); sit != client_info->m_sensors.end(); ++sit) {
        static_cast<SCA_CollisionSensor *>(*sit)->NewHandleCollision(ctrl1, ctrl2, nullptr);
      }
    }

    // Second client info
    client_info = static_cast<KX_ClientObjectInfo *>(ctrl2->GetNewClientInfo());
    // Second gameobject
    KX_GameObject *kxObj2 = KX_GameObject::GetClientObject(client_info);
    if (client_info) {
      for (sit = client_info->m_sensors.begin(); sit != client_info->m_sensors.end(); ++sit) {
        static_cast<SCA_CollisionSensor *>(*sit)->NewHandleCollision(ctrl2, ctrl1, nullptr);
      }
    }
    // Run python callbacks
    const PHY_ICollData *colldata = collision.colldata;
    KX_CollisionContactPointList contactPointList0 = KX_CollisionContactPointList(colldata, collision.isFirst);
    KX_CollisionContactPointList contactPointList1 = KX_CollisionContactPointList(colldata, !collision.isFirst);
    kxObj1->RunCollisionCallbacks(kxObj2, contactPointList0);
    kxObj2->RunCollisionCallbacks(kxObj1, contactPointList1);
  }

  for (SCA_ISensor *sensor : m_sensors) {
    sensor->Activate(m_logicmgr);
  }

  RemoveNewCollisions();
}

SCA_LogicManager *KX_CollisionEventManager::GetLogicManager()
{
  return m_logicmgr;
}

PHY_IPhysicsEnvironment *KX_CollisionEventManager::GetPhysicsEnvironment()
{
  return m_physEnv;
}

KX_CollisionEventManager::NewCollision::NewCollision(PHY_IPhysicsController *_first,
                                                     PHY_IPhysicsController *_second,
                                                     const PHY_ICollData *_colldata,
                                                     bool _isfirst)
    : first(_first), second(_second), colldata(_colldata), isFirst(_isfirst)
{
}

KX_CollisionEventManager::NewCollision::NewCollision(const NewCollision &to_copy)
    : first(to_copy.first), second(to_copy.second), colldata(to_copy.colldata), isFirst(to_copy.isFirst)
{
}

bool KX_CollisionEventManager::NewCollision::operator<(const NewCollision &other) const
{
  // see strict weak ordering: https://support.microsoft.com/en-us/kb/949171
  if (first == other.first) {
    if (second == other.second) {
      if (colldata == other.colldata) {
        return isFirst < other.isFirst;
      }
      return colldata < other.colldata;
    }
    return second < other.second;
  }
  return first < other.first;
}
