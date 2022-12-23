/*
 * Cast a ray and feel for objects
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

/** \file gameengine/Ketsji/SCA_RaySensor.cpp
 *  \ingroup ketsji
 */

#include "SCA_RaySensor.h"

#include "DNA_sensor_types.h"

#include "CM_Message.h"
#include "KX_ClientObjectInfo.h"
#include "KX_GameObject.h"
#include "KX_RayCast.h"
#include "RAS_MeshObject.h"

SCA_RaySensor::SCA_RaySensor(class SCA_EventManager *eventmgr,
                             SCA_IObject *gameobj,
                             const std::string &propname,
                             bool bFindMaterial,
                             bool bXRay,
                             double distance,
                             int axis,
                             int mask,
                             KX_Scene *ketsjiScene)
    : SCA_ISensor(gameobj, eventmgr),
      m_propertyname(propname),
      m_bFindMaterial(bFindMaterial),
      m_bXRay(bXRay),
      m_distance(distance),
      m_scene(ketsjiScene),
      m_axis(axis),
      m_mask(mask),
      m_hitMaterial("")
{
  Init();
}

void SCA_RaySensor::Init()
{
  m_bTriggered = (m_invert) ? true : false;
  m_rayHit = false;
  m_hitObject = nullptr;
  m_reset = true;
}

SCA_RaySensor::~SCA_RaySensor()
{
  /* Nothing to be done here. */
}

EXP_Value *SCA_RaySensor::GetReplica()
{
  SCA_RaySensor *replica = new SCA_RaySensor(*this);
  replica->ProcessReplica();
  replica->Init();

  return replica;
}

bool SCA_RaySensor::IsPositiveTrigger()
{
  bool result = m_rayHit;

  if (m_invert)
    result = !result;

  return result;
}

bool SCA_RaySensor::RayHit(KX_ClientObjectInfo *client, KX_RayCast *result, void */*data*/)
{

  KX_GameObject *hitKXObj = client->m_gameobject;
  bool bFound = false;
  std::string hitMaterial = "";

  if (m_propertyname.empty()) {
    bFound = true;
  }
  else {
    if (m_bFindMaterial) {
      for (unsigned int i = 0; i < hitKXObj->GetMeshCount(); ++i) {
        RAS_MeshObject *meshObj = hitKXObj->GetMesh(i);
        for (unsigned int j = 0; j < meshObj->NumMaterials(); ++j) {
          bFound = (m_propertyname == std::string(meshObj->GetMaterialName(j), 2));
          if (bFound) {
            hitMaterial = m_propertyname;
            break;
          }
        }
      }
    }
    else {
      bFound = hitKXObj->GetProperty(m_propertyname) != nullptr;
    }
  }

  if (bFound) {
    m_rayHit = true;
    m_hitObject = hitKXObj;
    m_hitPosition[0] = result->m_hitPoint[0];
    m_hitPosition[1] = result->m_hitPoint[1];
    m_hitPosition[2] = result->m_hitPoint[2];

    m_hitNormal[0] = result->m_hitNormal[0];
    m_hitNormal[1] = result->m_hitNormal[1];
    m_hitNormal[2] = result->m_hitNormal[2];

    m_hitMaterial = hitMaterial;
  }
  // no multi-hit search yet
  return true;
}

/* this function is used to pre-filter the object before casting the ray on them.
 * This is useful for "X-Ray" option when we want to see "through" unwanted object.
 */
bool SCA_RaySensor::NeedRayCast(KX_ClientObjectInfo *client, void */*data*/)
{
  KX_GameObject *hitKXObj = client->m_gameobject;

  if (client->m_type > KX_ClientObjectInfo::ACTOR) {
    // Unknown type of object, skip it.
    // Should not occur as the sensor objects are filtered in RayTest()
    CM_Error("invalid client type " << client->m_type << " found ray casting");
    return false;
  }

  // The current object is not in the proper layer.
  if (!(hitKXObj->GetCollisionGroup() & m_mask)) {
    return false;
  }

  if (m_bXRay && m_propertyname.size() != 0) {
    if (m_bFindMaterial) {
      bool found = false;
      for (unsigned int i = 0; i < hitKXObj->GetMeshCount(); ++i) {
        RAS_MeshObject *meshObj = hitKXObj->GetMesh(i);
        for (unsigned int j = 0; j < meshObj->NumMaterials(); ++j) {
          found = (m_propertyname == std::string(meshObj->GetMaterialName(j), 2));
          if (found)
            break;
        }
      }
      if (!found)
        return false;
    }
    else {
      if (hitKXObj->GetProperty(m_propertyname) == nullptr)
        return false;
    }
  }
  return true;
}

bool SCA_RaySensor::Evaluate()
{
  bool result = false;
  bool reset = m_reset && m_level;
  m_rayHit = false;
  m_hitObject = nullptr;
  m_hitPosition[0] = 0;
  m_hitPosition[1] = 0;
  m_hitPosition[2] = 0;

  m_hitNormal[0] = 1;
  m_hitNormal[1] = 0;
  m_hitNormal[2] = 0;

  KX_GameObject *obj = (KX_GameObject *)GetParent();
  MT_Vector3 frompoint = obj->NodeGetWorldPosition();
  MT_Matrix3x3 matje = obj->NodeGetWorldOrientation();
  MT_Matrix3x3 invmat = matje.inverse();

  MT_Vector3 todir;
  m_reset = false;
  switch (m_axis) {
    case SENS_RAY_X_AXIS:  // X
    {
      todir[0] = invmat[0][0];
      todir[1] = invmat[0][1];
      todir[2] = invmat[0][2];
      break;
    }
    case SENS_RAY_Y_AXIS:  // Y
    {
      todir[0] = invmat[1][0];
      todir[1] = invmat[1][1];
      todir[2] = invmat[1][2];
      break;
    }
    case SENS_RAY_Z_AXIS:  // Z
    {
      todir[0] = invmat[2][0];
      todir[1] = invmat[2][1];
      todir[2] = invmat[2][2];
      break;
    }
    case SENS_RAY_NEG_X_AXIS:  // -X
    {
      todir[0] = -invmat[0][0];
      todir[1] = -invmat[0][1];
      todir[2] = -invmat[0][2];
      break;
    }
    case SENS_RAY_NEG_Y_AXIS:  // -Y
    {
      todir[0] = -invmat[1][0];
      todir[1] = -invmat[1][1];
      todir[2] = -invmat[1][2];
      break;
    }
    case SENS_RAY_NEG_Z_AXIS:  // -Z
    {
      todir[0] = -invmat[2][0];
      todir[1] = -invmat[2][1];
      todir[2] = -invmat[2][2];
      break;
    }
  }
  todir.normalize();
  m_rayDirection[0] = todir[0];
  m_rayDirection[1] = todir[1];
  m_rayDirection[2] = todir[2];

  MT_Vector3 topoint = frompoint + (m_distance)*todir;
  PHY_IPhysicsEnvironment *pe = m_scene->GetPhysicsEnvironment();

  if (!pe) {
    CM_LogicBrickWarning(this, "there is no physics environment! Check universe for malfunction.");
    return false;
  }

  PHY_IPhysicsController *spc = obj->GetPhysicsController();
  KX_GameObject *parent = obj->GetParent();
  if (!spc && parent)
    spc = parent->GetPhysicsController();

  PHY_IPhysicsEnvironment *physics_environment = this->m_scene->GetPhysicsEnvironment();

  KX_RayCast::Callback<SCA_RaySensor, void> callback(this, spc);
  KX_RayCast::RayTest(physics_environment, frompoint, topoint, callback);

  /* now pass this result to some controller */

  if (m_rayHit) {
    if (!m_bTriggered) {
      // notify logicsystem that ray is now hitting
      result = true;
      m_bTriggered = true;
    }
    else {
      // notify logicsystem that ray is STILL hitting ...
      result = false;
    }
  }
  else {
    if (m_bTriggered) {
      m_bTriggered = false;
      // notify logicsystem that ray JUST left the Object
      result = true;
    }
    else {
      result = false;
    }
  }
  if (reset)
    // force an event
    result = true;

  return result;
}

#ifdef WITH_PYTHON

/* ------------------------------------------------------------------------- */
/* Python functions                                                          */
/* ------------------------------------------------------------------------- */

/* Integration hooks ------------------------------------------------------- */
PyTypeObject SCA_RaySensor::Type = {PyVarObject_HEAD_INIT(nullptr, 0) "SCA_RaySensor",
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
                                    py_base_new

};

PyMethodDef SCA_RaySensor::Methods[] = {
    {nullptr, nullptr}  // Sentinel
};

PyAttributeDef SCA_RaySensor::Attributes[] = {
    EXP_PYATTRIBUTE_BOOL_RW("useMaterial", SCA_RaySensor, m_bFindMaterial),
    EXP_PYATTRIBUTE_BOOL_RW("useXRay", SCA_RaySensor, m_bXRay),
    EXP_PYATTRIBUTE_FLOAT_RW("range", 0, 10000, SCA_RaySensor, m_distance),
    EXP_PYATTRIBUTE_STRING_RW("propName", 0, MAX_PROP_NAME, false, SCA_RaySensor, m_propertyname),
    EXP_PYATTRIBUTE_INT_RW("axis", 0, 5, true, SCA_RaySensor, m_axis),
    EXP_PYATTRIBUTE_INT_RW("mask", 1, (1 << OB_MAX_COL_MASKS) - 1, true, SCA_RaySensor, m_mask),
    EXP_PYATTRIBUTE_FLOAT_ARRAY_RO("hitPosition", SCA_RaySensor, m_hitPosition, 3),
    EXP_PYATTRIBUTE_FLOAT_ARRAY_RO("rayDirection", SCA_RaySensor, m_rayDirection, 3),
    EXP_PYATTRIBUTE_FLOAT_ARRAY_RO("hitNormal", SCA_RaySensor, m_hitNormal, 3),
    EXP_PYATTRIBUTE_STRING_RO("hitMaterial", SCA_RaySensor, m_hitMaterial),
    EXP_PYATTRIBUTE_RO_FUNCTION("hitObject", SCA_RaySensor, pyattr_get_hitobject),
    EXP_PYATTRIBUTE_NULL  // Sentinel
};

PyObject *SCA_RaySensor::pyattr_get_hitobject(EXP_PyObjectPlus *self_v,
                                              const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_RaySensor *self = static_cast<SCA_RaySensor *>(self_v);
  if (self->m_hitObject)
    return self->m_hitObject->GetProxy();

  Py_RETURN_NONE;
}

#endif  // WITH_PYTHON
