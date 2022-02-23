/*
 * Senses touch and collision events
 *
 *
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

/** \file gameengine/Ketsji/SCA_CollisionSensor.cpp
 *  \ingroup ketsji
 */

#include "SCA_CollisionSensor.h"

#include "KX_CollisionEventManager.h"
#include "PHY_IPhysicsController.h"
#include "PHY_IPhysicsEnvironment.h"
#include "RAS_MeshObject.h"

/* ------------------------------------------------------------------------- */
/* Native functions                                                          */
/* ------------------------------------------------------------------------- */

void SCA_CollisionSensor::SynchronizeTransform()
{
  // the touch sensor does not require any synchronization: it uses
  // the same physical object which is already synchronized by Blender
}

void SCA_CollisionSensor::EndFrame()
{
  m_colliders->ReleaseAndRemoveAll();
  m_hitObject = nullptr;
  m_bTriggered = false;
  m_bColliderHash = 0;
}

void SCA_CollisionSensor::UnregisterToManager()
{
  // before unregistering the sensor, make sure we release all references
  EndFrame();
  SCA_ISensor::UnregisterToManager();
}

bool SCA_CollisionSensor::Evaluate()
{
  bool result = false;
  bool reset = m_reset && m_level;
  m_reset = false;
  if (m_bTriggered != m_bLastTriggered) {
    m_bLastTriggered = m_bTriggered;
    if (!m_bTriggered) {
      m_hitObject = nullptr;
    }
    result = true;
  }
  if (reset) {
    // force an event
    result = true;
  }

  if (m_bCollisionPulse) {  // pulse on changes to the colliders
    int count = m_colliders->GetCount();

    if (m_bLastCount != count || m_bColliderHash != m_bLastColliderHash) {
      m_bLastCount = count;
      m_bLastColliderHash = m_bColliderHash;
      result = true;
    }
  }
  return result;
}

SCA_CollisionSensor::SCA_CollisionSensor(SCA_EventManager *eventmgr,
                                         KX_GameObject *gameobj,
                                         bool bFindMaterial,
                                         bool bCollisionPulse,
                                         const std::string &touchedpropname)
    : SCA_ISensor(gameobj, eventmgr),
      m_touchedpropname(touchedpropname),
      m_bFindMaterial(bFindMaterial),
      m_bCollisionPulse(bCollisionPulse),
      m_hitMaterial("")
{
  m_colliders = new EXP_ListValue<KX_GameObject>();

  KX_ClientObjectInfo *client_info = gameobj->getClientInfo();
  client_info->m_sensors.push_back(this);

  m_physCtrl = gameobj->GetPhysicsController();
  BLI_assert(!gameobj->GetPhysicsController() || m_physCtrl);
  Init();
}

void SCA_CollisionSensor::Init()
{
  m_bCollision = false;
  m_bTriggered = false;
  m_bLastTriggered = (m_invert) ? true : false;
  m_bLastCount = 0;
  m_bColliderHash = m_bLastColliderHash = 0;
  m_hitObject = nullptr;
  m_reset = true;
}

SCA_CollisionSensor::~SCA_CollisionSensor()
{
  m_colliders->Release();
}

EXP_Value *SCA_CollisionSensor::GetReplica()
{
  SCA_CollisionSensor *replica = new SCA_CollisionSensor(*this);
  replica->ProcessReplica();
  return replica;
}

void SCA_CollisionSensor::ProcessReplica()
{
  SCA_ISensor::ProcessReplica();
  m_colliders = new EXP_ListValue<KX_GameObject>();
  Init();
}

void SCA_CollisionSensor::ReParent(SCA_IObject *parent)
{
  KX_GameObject *gameobj = static_cast<KX_GameObject *>(parent);
  PHY_IPhysicsController *sphy = ((KX_GameObject *)parent)->GetPhysicsController();
  if (sphy) {
    m_physCtrl = sphy;
  }

  KX_ClientObjectInfo *client_info = gameobj->getClientInfo();

  client_info->m_sensors.push_back(this);
  SCA_ISensor::ReParent(parent);
}

void SCA_CollisionSensor::RegisterSumo(KX_CollisionEventManager *collisionman)
{
  if (m_physCtrl) {
    if (collisionman->GetPhysicsEnvironment()->RequestCollisionCallback(m_physCtrl)) {
      KX_ClientObjectInfo *client_info = static_cast<KX_ClientObjectInfo *>(
          m_physCtrl->GetNewClientInfo());
      if (client_info->isSensor()) {
        collisionman->GetPhysicsEnvironment()->AddSensor(m_physCtrl);
      }
    }
  }
}
void SCA_CollisionSensor::UnregisterSumo(KX_CollisionEventManager *collisionman)
{
  if (m_physCtrl) {
    if (collisionman->GetPhysicsEnvironment()->RemoveCollisionCallback(m_physCtrl)) {
      // no more sensor on the controller, can remove it if it is a sensor object
      KX_ClientObjectInfo *client_info = static_cast<KX_ClientObjectInfo *>(
          m_physCtrl->GetNewClientInfo());
      if (client_info->isSensor()) {
        collisionman->GetPhysicsEnvironment()->RemoveSensor(m_physCtrl);
      }
    }
  }
}

// this function is called only for sensor objects
// return true if the controller can collide with the object
bool SCA_CollisionSensor::BroadPhaseSensorFilterCollision(PHY_IPhysicsController *ctrl1, PHY_IPhysicsController *ctrl2)
{
  BLI_assert(ctrl1 == m_physCtrl && ctrl2);

  KX_GameObject *myobj = (KX_GameObject *)GetParent();
  KX_GameObject *myparent = myobj->GetParent();
  KX_ClientObjectInfo *client_info = static_cast<KX_ClientObjectInfo *>(ctrl2->GetNewClientInfo());
  KX_ClientObjectInfo *my_client_info = static_cast<KX_ClientObjectInfo *>(m_physCtrl->GetNewClientInfo());
  KX_GameObject *otherobj = (client_info ? client_info->m_gameobject : nullptr);

  // we can only check on persistent characteristic: m_link and m_suspended are not
  // good candidate because they are transient. That must be handled at another level
  if (!otherobj || otherobj == myparent ||  // don't interact with our parent
      (my_client_info->m_type == KX_ClientObjectInfo::OBACTORSENSOR &&
       client_info->m_type != KX_ClientObjectInfo::ACTOR))  // only with actor objects
  {
    return false;
  }

  bool found = m_touchedpropname.empty();
  if (!found) {
    if (m_bFindMaterial) {
      for (unsigned int i = 0; i < otherobj->GetMeshCount(); ++i) {
        RAS_MeshObject *meshObj = otherobj->GetMesh(i);
        for (unsigned int j = 0; j < meshObj->NumMaterials(); ++j) {
          found = (m_touchedpropname == std::string(meshObj->GetMaterialName(j), 2));
          if (found) {
            break;
          }
        }
      }
    }
    else {
      found = (otherobj->GetProperty(m_touchedpropname) != nullptr);
    }
  }
  return found;
}

bool SCA_CollisionSensor::NewHandleCollision(PHY_IPhysicsController *ctrl1,
                                             PHY_IPhysicsController *ctrl2,
                                             const PHY_ICollData *colldata)
{
  KX_GameObject *parent = (KX_GameObject *)GetParent();

  // need the mapping from PHY_IPhysicsController to gameobjects now

  KX_ClientObjectInfo *client_info = static_cast<KX_ClientObjectInfo *> (ctrl1 == m_physCtrl ?
                                                                         ctrl2->GetNewClientInfo() : ctrl1->GetNewClientInfo());

  KX_GameObject *gameobj = (client_info ? client_info->m_gameobject : nullptr);

  // add the same check as in SCA_ISensor::Activate(),
  // we don't want to record collision when the sensor is not active.
  if (m_links && !m_suspended && gameobj && (gameobj != parent) && client_info->isActor()) {

    bool found = m_touchedpropname.empty();
    std::string hitMaterial = "";
    if (!found) {
      if (m_bFindMaterial) {
        for (unsigned int i = 0; i < gameobj->GetMeshCount(); ++i) {
          RAS_MeshObject *meshObj = gameobj->GetMesh(i);
          for (unsigned int j = 0; j < meshObj->NumMaterials(); ++j) {
            found = (m_touchedpropname == std::string(meshObj->GetMaterialName(j), 2));
            if (found) {
              hitMaterial = m_touchedpropname;
              break;
            }
          }
        }
      }
      else {
        found = (gameobj->GetProperty(m_touchedpropname) != nullptr);
      }
    }
    if (found) {
      if (!m_colliders->SearchValue(gameobj)) {
        m_colliders->Add(CM_AddRef(gameobj));

        if (m_bCollisionPulse) {
          m_bColliderHash += (uint_ptr)(static_cast<void *>(&gameobj));
        }
      }
      m_bTriggered = true;
      m_hitObject = gameobj;
      m_hitMaterial = hitMaterial;
    }
  }
  return false;
}

#ifdef WITH_PYTHON

/* ------------------------------------------------------------------------- */
/* Python functions                                                          */
/* ------------------------------------------------------------------------- */
/* Integration hooks ------------------------------------------------------- */
PyTypeObject SCA_CollisionSensor::Type = {PyVarObject_HEAD_INIT(nullptr, 0) "SCA_CollisionSensor",
                                          sizeof(EXP_PyObjectPlus_Proxy),
                                          0,
                                          py_base_dealloc,
                                          0,
                                          0,
                                          0,
                                          0,
                                          py_base_repr,
                                          0,
                                          0,
                                          0,
                                          0,
                                          0,
                                          0,
                                          0,
                                          0,
                                          0,
                                          Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
                                          0,
                                          0,
                                          0,
                                          0,
                                          0,
                                          0,
                                          0,
                                          Methods,
                                          0,
                                          0,
                                          &SCA_ISensor::Type,
                                          0,
                                          0,
                                          0,
                                          0,
                                          0,
                                          0,
                                          py_base_new};

PyMethodDef SCA_CollisionSensor::Methods[] = {
    {nullptr, nullptr}  // Sentinel
};

PyAttributeDef SCA_CollisionSensor::Attributes[] = {
    EXP_PYATTRIBUTE_STRING_RW(
        "propName", 0, MAX_PROP_NAME, false, SCA_CollisionSensor, m_touchedpropname),
    EXP_PYATTRIBUTE_BOOL_RW("useMaterial", SCA_CollisionSensor, m_bFindMaterial),
    EXP_PYATTRIBUTE_BOOL_RW("usePulseCollision", SCA_CollisionSensor, m_bCollisionPulse),
    EXP_PYATTRIBUTE_STRING_RO("hitMaterial", SCA_CollisionSensor, m_hitMaterial),
    EXP_PYATTRIBUTE_RO_FUNCTION("hitObject", SCA_CollisionSensor, pyattr_get_object_hit),
    EXP_PYATTRIBUTE_RO_FUNCTION("hitObjectList", SCA_CollisionSensor, pyattr_get_object_hit_list),
    EXP_PYATTRIBUTE_NULL  // Sentinel
};

/* Python API */

PyObject *SCA_CollisionSensor::pyattr_get_object_hit(EXP_PyObjectPlus *self_v,
                                                     const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_CollisionSensor *self = static_cast<SCA_CollisionSensor *>(self_v);

  if (self->m_hitObject) {
    return self->m_hitObject->GetProxy();
  }
  else {
    Py_RETURN_NONE;
  }
}

PyObject *SCA_CollisionSensor::pyattr_get_object_hit_list(EXP_PyObjectPlus *self_v,
                                                          const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_CollisionSensor *self = static_cast<SCA_CollisionSensor *>(self_v);
  return self->m_colliders->GetProxy();
}

#endif
