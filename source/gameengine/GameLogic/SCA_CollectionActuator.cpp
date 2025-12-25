/*
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

/** \file gameengine/Ketsji/SCA_CollectionActuator.cpp
 *  \ingroup ketsji
 */

#include "SCA_CollectionActuator.h"

#include "KX_Camera.h"

#include "BKE_collection.hh"
#include "PHY_IPhysicsController.h"

#include <unordered_map>
#include <vector>

/* ------------------------------------------------------------------------- */
/* Native functions                                                          */
/* ------------------------------------------------------------------------- */

SCA_CollectionActuator::SCA_CollectionActuator(SCA_IObject *gameobj,
                                               KX_Scene *scene,
                                               KX_Camera *cam,
                                               Collection *collection,
                                               int mode,
                                               bool use_logic,
                                               bool use_physics,
                                               bool use_visibility,
                                               bool full_copy,
                                               const float *linvel,
                                               bool linv_local,
                                               const float *angvel,
                                               bool angv_local)
    : SCA_IActuator(gameobj, KX_ACT_COLLECTION),
      m_kxscene(scene),
      m_collection(collection),
      m_camera(cam),
      m_mode(mode),
      m_useLogic(use_logic),
      m_usePhysics(use_physics),
      m_useVisibility(use_visibility),
      m_fullCopy(full_copy),
      m_localLinvFlag(linv_local),
      m_localAngvFlag(angv_local)
{
  m_linear_velocity[0] = linvel[0];
  m_linear_velocity[1] = linvel[1];
  m_linear_velocity[2] = linvel[2];

  m_angular_velocity[0] = angvel[0];
  m_angular_velocity[1] = angvel[1];
  m_angular_velocity[2] = angvel[2];

  if (m_camera)
    m_camera->RegisterActuator(this);
} /* End of constructor */

SCA_CollectionActuator::~SCA_CollectionActuator()
{
  if (m_camera)
    m_camera->UnregisterActuator(this);
} /* end of destructor */

EXP_Value *SCA_CollectionActuator::GetReplica()
{
  SCA_CollectionActuator *replica = new SCA_CollectionActuator(*this);
  replica->ProcessReplica();
  return replica;
}

void SCA_CollectionActuator::ProcessReplica()
{
  if (m_camera)
    m_camera->RegisterActuator(this);
  SCA_IActuator::ProcessReplica();
}

bool SCA_CollectionActuator::UnlinkObject(SCA_IObject *clientobj)
{
  if (clientobj == (SCA_IObject *)m_camera) {
    // this object is being deleted, we cannot continue to use it.
    m_camera = nullptr;
    return true;
  }
  return false;
}

void SCA_CollectionActuator::Relink(std::map<SCA_IObject *, SCA_IObject *> &obj_map)
{
  KX_Camera *obj = static_cast<KX_Camera *>(obj_map[m_camera]);
  if (obj) {
    if (m_camera)
      m_camera->UnregisterActuator(this);
    m_camera = obj;
    m_camera->RegisterActuator(this);
  }
}

bool SCA_CollectionActuator::Update()
{
  // bool result = false;	/*unused*/
  bool bNegativeEvent = IsNegativeEvent();
  RemoveAllEvents();

  if (bNegativeEvent)
    return false;  // do nothing on negative events

  switch (m_mode) {
    case KX_COLLECTION_SUSPEND:
      for (KX_GameObject *gameobj : m_kxscene->GetObjectList()) {
        Object *ob = gameobj->GetBlenderObject();
        if (ob && BKE_collection_has_object(m_collection, ob)) {
          if (m_useLogic) {
            gameobj->SuspendLogicAndActions(false);
          }
          if (m_usePhysics) {
            gameobj->SuspendPhysics(false, false);
          }
          if (m_useVisibility) {
            gameobj->SetVisible(false, false);
          }
        }
      }
      break;
    case KX_COLLECTION_RESUME:
      for (KX_GameObject *gameobj : m_kxscene->GetObjectList()) {
        Object *ob = gameobj->GetBlenderObject();
        if (ob && BKE_collection_has_object(m_collection, ob)) {
          if (m_useLogic) {
            gameobj->RestoreLogicAndActions(false);
          }
          if (m_usePhysics) {
            gameobj->RestorePhysics(false);
          }
          if (m_useVisibility) {
            gameobj->SetVisible(true, false);
          }
        }
      }
      break;
    case KX_COLLECTION_ADD_OVERLAY:
      if (m_camera) {
        m_kxscene->AddOverlayCollection(m_camera, m_collection);
      }
      else {
        std::cout << "Collection Actuator: Camera not found" << std::endl;
      }
      break;
    case KX_COLLECTION_REMOVE_OVERLAY:
      m_kxscene->RemoveOverlayCollection(m_collection);
      break;
    case KX_COLLECTION_SPAWN: {
      KX_GameObject *referenceobj = static_cast<KX_GameObject *>(GetParent());
      std::vector<KX_GameObject *> spawned_objects;
      std::vector<KX_GameObject *> spawned_physics;
      std::unordered_map<std::string, KX_GameObject *> spawned_lookup;

      for (KX_GameObject *gameobj : m_kxscene->GetInactiveList()) {
        Object *ob = gameobj->GetBlenderObject();
        if (ob && BKE_collection_has_object(m_collection, ob)) {
          KX_GameObject *replica = nullptr;
          if (!m_fullCopy) {
            replica = m_kxscene->AddReplicaObject(gameobj, nullptr, 0.0f);
          }
          else {
            replica = m_kxscene->AddFullCopyObject(gameobj, nullptr, 0.0f);
          }
          if (replica) {
            if (referenceobj) {
              /* Compose original transform with owner transform so each spawned object keeps its
               * collection offset relative to the actuator owner, similar to instanced collections.
               */
              const MT_Vector3 owner_scale =
                  referenceobj->GetSGNode()->GetRootSGParent()->GetLocalScale();
              const MT_Vector3 orig_pos = replica->NodeGetWorldPosition();
              const MT_Matrix3x3 orig_ori = replica->NodeGetWorldOrientation();
              const MT_Vector3 orig_scale = replica->GetSGNode()->GetRootSGParent()->GetLocalScale();

              const MT_Matrix3x3 owner_ori = referenceobj->NodeGetWorldOrientation();
              const MT_Vector3 owner_pos = referenceobj->NodeGetWorldPosition();

              const MT_Vector3 scaled_offset(orig_pos[0] * owner_scale[0],
                                             orig_pos[1] * owner_scale[1],
                                             orig_pos[2] * owner_scale[2]);
              const MT_Vector3 composed_pos = owner_pos + owner_ori * scaled_offset;
              const MT_Matrix3x3 composed_ori = owner_ori * orig_ori;
              const MT_Vector3 composed_scale(orig_scale[0] * owner_scale[0],
                                              orig_scale[1] * owner_scale[1],
                                              orig_scale[2] * owner_scale[2]);

              replica->NodeSetLocalPosition(composed_pos);
              replica->NodeSetLocalOrientation(composed_ori);
              replica->NodeSetRelativeScale(composed_scale);
              replica->GetSGNode()->UpdateWorldData(0.0);
            }
            replica->setLinearVelocity(MT_Vector3(m_linear_velocity), m_localLinvFlag);
            replica->setAngularVelocity(MT_Vector3(m_angular_velocity), m_localAngvFlag);

            spawned_lookup.emplace(gameobj->GetName(), replica);
            /* Use both game object and Blender ID names to maximize match chances after full copy. */
            spawned_lookup.emplace(gameobj->GetBlenderObject()->id.name + 2, replica);
            /* Full copy may rename the new object (e.g. Cube.001), so also map by the replica name
             * (game object name and Blender ID name) to resolve constraints correctly when target
             * names changed on duplication.
             */
            spawned_lookup.emplace(replica->GetName(), replica);
            spawned_lookup.emplace(replica->GetBlenderObject()->id.name + 2, replica);
            spawned_objects.push_back(replica);
            if (replica->GetPhysicsController()) {
              spawned_physics.push_back(replica);
            }
          }
        }
      }

      /* Replicate physics constraints between spawned objects using shared lookup. */
      for (KX_GameObject *replica : spawned_physics) {
        replica->GetPhysicsController()->ReplicateConstraints(replica, spawned_physics);
        replica->ClearConstraints();
      }

      /* Replicate rigid body constraints using name-based lookup, similar to hierarchy duplication. */
      for (KX_GameObject *replica : spawned_objects) {
        if (replica->HasRigidBodyConstraints()) {
          replica->ReplicateRigidBodyConstraints(spawned_lookup);
        }
      }

      if (!m_fullCopy) {
        for (KX_GameObject *replica : spawned_objects) {
          replica->Release();
        }
      }
      break;
    }
    default:
      break;
  }

  return false;
}

#ifdef WITH_PYTHON

/* ------------------------------------------------------------------------- */
/* Python functions                                                          */
/* ------------------------------------------------------------------------- */

/* Integration hooks ------------------------------------------------------- */
PyTypeObject SCA_CollectionActuator::Type = {
    PyVarObject_HEAD_INIT(nullptr, 0) "SCA_CollectionActuator",
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
    &SCA_IActuator::Type,
    0,
    0,
    0,
    0,
    0,
    0,
    py_base_new};

PyMethodDef SCA_CollectionActuator::Methods[] = {
    {nullptr, nullptr}  // Sentinel
};

PyAttributeDef SCA_CollectionActuator::Attributes[] = {
    // EXP_PYATTRIBUTE_STRING_RW("scene",0,MAX_ID_NAME-2,true,SCA_SceneActuator,m_nextSceneName),
    // EXP_PYATTRIBUTE_RW_FUNCTION("camera",SCA_SceneActuator,pyattr_get_camera,pyattr_set_camera),
    // EXP_PYATTRIBUTE_BOOL_RW("useRestart", SCA_SceneActuator, m_restart),
    // EXP_PYATTRIBUTE_INT_RW("mode", KX_SCENE_NODEF+1, KX_SCENE_MAX-1, true, SCA_SceneActuator,
    // m_mode),
    EXP_PYATTRIBUTE_NULL  // Sentinel
};

#endif  // WITH_PYTHON

/* eof */
