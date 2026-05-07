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

#include <unordered_map>
#include <vector>

#include "KX_Camera.h"
#include "KX_GameObject.h"
#include "KX_Scene.h"

#include "BKE_collection.hh"
#include "MT_Matrix3x3.h"
#include "MT_Vector3.h"
#include "PHY_IPhysicsController.h"

using namespace blender;

/* ------------------------------------------------------------------------- */
/* Native functions                                                          */
/* ------------------------------------------------------------------------- */

SCA_CollectionActuator::SCA_CollectionActuator(SCA_IObject *gameobj,
                                               KX_Scene *scene,
                                               KX_Camera *cam,
                                               blender::Collection *collection,
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
        blender::Object *ob = gameobj->GetBlenderObject();
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
        blender::Object *ob = gameobj->GetBlenderObject();
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
      const MT_Vector3 collection_offset(m_collection->instance_offset);
      const bool use_first_object_as_origin = collection_offset.fuzzyZero();
      MT_Vector3 spawn_origin = collection_offset;
      bool spawn_origin_initialized = !use_first_object_as_origin;

      for (KX_GameObject *gameobj : m_kxscene->GetInactiveList()) {
        blender::Object *ob = gameobj->GetBlenderObject();
        if (ob && BKE_collection_has_object(m_collection, ob)) {
          /* Skip child objects: AddReplicaObject already spawns the full
           * hierarchy when the parent is processed.  Spawning children
           * individually here would create duplicate stray objects (the
           * classic "phantom soft body at template position" bug). */
          KX_GameObject *templateParent = gameobj->GetParent();
          if (templateParent) {
            blender::Object *parentOb = templateParent->GetBlenderObject();
            if (parentOb && BKE_collection_has_object(m_collection, parentOb)) {
              continue;
            }
          }

          KX_GameObject *replica = nullptr;
          if (!m_fullCopy) {
            replica = m_kxscene->AddReplicaObject(gameobj, nullptr, 0.0f);
          }
          else {
            replica = m_kxscene->AddFullCopyObject(gameobj, nullptr, 0.0f);
            /* Full copy converts each object individually, so the conversion's
             * local sumolist can't resolve cross-object references. Copy the
             * constraint data from the original inactive template so that
             * ReplicateConstraints and ReplicateRigidBodyConstraints work. */
            if (replica) {
              for (const KX_GameObject::RigidBodyConstraintData &origData :
                   gameobj->GetRigidBodyConstraints())
              {
                KX_GameObject::RigidBodyConstraintData newData = origData;
                newData.m_constraintId = -1;
                replica->m_rigidbodyConstraints.push_back(newData);
              }
              for (blender::bRigidBodyJointConstraint *cons : gameobj->GetConstraints()) {
                replica->AddConstraint(cons);
              }
            }
          }
          if (replica) {
            if (referenceobj) {
              /* Spawn relative to collection origin at the actuator owner transform.
               * This mirrors collection instancing behavior and keeps per-object offsets.
               */
              if (!spawn_origin_initialized) {
                /* Keep default behavior with collection.instance_offset when it is set.
                 * Otherwise, anchor to the first collection object so the spawn starts
                 * exactly at the logic brick owner position.
                 */
                spawn_origin = gameobj->NodeGetWorldPosition();
                spawn_origin_initialized = true;
              }

              const MT_Vector3 owner_scale = referenceobj->NodeGetWorldScaling();
              const MT_Vector3 orig_pos = gameobj->NodeGetWorldPosition();
              const MT_Matrix3x3 orig_ori = gameobj->NodeGetWorldOrientation();

              const MT_Matrix3x3 owner_ori = referenceobj->NodeGetWorldOrientation();
              const MT_Vector3 owner_pos = referenceobj->NodeGetWorldPosition();

              const MT_Vector3 relative_pos = orig_pos - spawn_origin;

              const MT_Vector3 scaled_offset(relative_pos[0] * owner_scale[0],
                                             relative_pos[1] * owner_scale[1],
                                             relative_pos[2] * owner_scale[2]);
              const MT_Vector3 composed_pos = owner_pos + owner_ori * scaled_offset;
              const MT_Matrix3x3 composed_ori = owner_ori * orig_ori;

              replica->NodeSetLocalPosition(composed_pos);
              replica->NodeSetLocalOrientation(composed_ori);
              replica->NodeSetRelativeScale(owner_scale);
              replica->GetSGNode()->UpdateWorldData(0.0);

              /* Re-sync all physics bodies in the hierarchy to the new world
               * positions.  AddReplicaObject called SetTransform() while the
               * SGNode was still at the template position; soft-body deferred
               * clones are created inside SetTransform(), so they ended up at
               * the wrong location.  A second pass after UpdateWorldData places
               * them correctly.  m_pendingSoftBodySource is consumed on the
               * first call, so the second call only does a cheap position
               * update for rigid bodies and a proper clone creation for any
               * soft body that still has a pending source (there shouldn't be
               * any after the first pass, but the guard inside SetTransform
               * handles that safely). */
              std::function<void(KX_GameObject *)> syncPhysics =
                  [&](KX_GameObject *obj) {
                    if (obj->GetPhysicsController()) {
                      obj->GetPhysicsController()->SetTransform();
                    }
                    for (KX_GameObject *child : obj->GetChildren()) {
                      syncPhysics(child);
                    }
                  };
              syncPhysics(replica);
            }
            /* Compute world-space velocity from local flag and actuator owner rotation. */
            MT_Vector3 worldLinVel(m_linear_velocity);
            MT_Vector3 worldAngVel(m_angular_velocity);
            if (m_localLinvFlag || m_localAngvFlag) {
              KX_GameObject *owner = static_cast<KX_GameObject *>(GetParent());
              MT_Matrix3x3 ownerOri = owner->NodeGetWorldOrientation();
              if (m_localLinvFlag) {
                worldLinVel = ownerOri * worldLinVel;
              }
              if (m_localAngvFlag) {
                worldAngVel = ownerOri * worldAngVel;
              }
            }
            replica->setLinearVelocity(worldLinVel, false);
            replica->setAngularVelocity(worldAngVel, false);
            /* Propagate linear velocity to children (e.g. soft body pinned to this RB). */
            for (KX_GameObject *child : replica->GetChildren()) {
              child->setLinearVelocity(worldLinVel, false);
            }

            /* Rigid body constraint replication looks up targets by Blender ID names
             * (ob->id.name + 2). Logic names or replica names are never queried, so
             * avoid storing redundant entries in the lookup table. */
            spawned_lookup.emplace(gameobj->GetBlenderObject()->id.name + 2, replica);
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
