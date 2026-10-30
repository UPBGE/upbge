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

/** \file gameengine/Converter/BL_SceneConverter.cpp
 *  \ingroup bgeconv
 */

#include "BL_SceneConverter.h"

#include "KX_BlenderMaterial.h"
#include "KX_GameObject.h"
#include "PHY_IPhysicsController.h"

using namespace blender;

BL_SceneConverter::BL_SceneConverter()
{
  m_materials = {};
  m_meshobjects = {};
  m_map_blender_to_gameobject = {};
  m_map_mesh_to_gamemesh = {};
  m_map_mesh_to_polyaterial = {};
  m_map_blender_to_gameactuator = {};
  m_map_blender_to_gamecontroller = {};
  m_pending_parent_suspends = {};
  m_pending_jolt_transform_parenting = {};
}

BL_SceneConverter::~BL_SceneConverter()
{
  m_materials.clear();
  m_owned_runtime_materials.clear();
  m_meshobjects.clear();
  m_map_blender_to_gameobject.clear();
  m_map_mesh_to_gamemesh.clear();
  m_map_mesh_to_polyaterial.clear();
  m_map_blender_to_gameactuator.clear();
  m_map_blender_to_gamecontroller.clear();
  m_pending_parent_suspends.clear();
  m_pending_jolt_transform_parenting.clear();
}

void BL_SceneConverter::RegisterGameObject(KX_GameObject *gameobject,
                                                  blender::Object *for_blenderobject)
{
  if (gameobject->IsUpbgeDupliInstance() && !gameobject->IsShared()) {
    return;
  }
  // 	CM_FunctionDebug("object name: " << gameobject->GetName());
  // only maintained while converting, freed during game runtime (not anymore in 0.3)
  std::map<blender::Object *, KX_GameObject *>::iterator it = m_map_blender_to_gameobject.find(for_blenderobject);
  if (it != m_map_blender_to_gameobject.end() && it->second == gameobject) {
    std::cout << "Warning: You're trying to register the same blender::Object several times" << std::endl;
    return;
  }
  m_map_blender_to_gameobject[for_blenderobject] = gameobject;
}

/** only need to run this during conversion since
 * m_map_blender_to_gameobject is freed after conversion (not anymore in 0.3) */
void BL_SceneConverter::UnregisterGameObject(KX_GameObject *gameobject)
{
  /* A grouped Auto object (Game Drawing) has its own Blender object. */
  if (gameobject->IsUpbgeDupliInstance() && !gameobject->IsShared()) {
    return; // We want blenderObject dupli Base to remain accessible
  }
  blender::Object *bobp = gameobject->GetBlenderObject();
  if (bobp) {
    std::map<blender::Object *, KX_GameObject *>::iterator it = m_map_blender_to_gameobject.find(bobp);
    if (it != m_map_blender_to_gameobject.end() && it->second == gameobject) {
      // also maintain m_map_blender_to_gameobject if the gameobject
      // being removed is matching the blender object
      m_map_blender_to_gameobject.erase(it);
    }
  }
}

KX_GameObject *BL_SceneConverter::FindGameObject(blender::Object *for_blenderobject)
{
  return m_map_blender_to_gameobject[for_blenderobject];
}

void BL_SceneConverter::RegisterGameMesh(RAS_MeshObject *gamemesh, blender::Mesh *for_blendermesh)
{
  if (for_blendermesh) {  // dynamically loaded meshes we don't want to keep lookups for
    m_map_mesh_to_gamemesh[for_blendermesh] = gamemesh;
  }
  m_meshobjects.push_back(gamemesh);
}

RAS_MeshObject *BL_SceneConverter::FindGameMesh(blender::Mesh *for_blendermesh)
{
  return m_map_mesh_to_gamemesh[for_blendermesh];
}

void BL_SceneConverter::RegisterMaterial(KX_BlenderMaterial *blmat, blender::Material *mat)
{
  if (mat) {
    m_map_mesh_to_polyaterial[mat] = blmat;
  }
  m_materials.push_back(blmat);
}

KX_BlenderMaterial *BL_SceneConverter::OwnRuntimeMaterial(
    std::unique_ptr<KX_BlenderMaterial> blmat, blender::Material *mat)
{
  KX_BlenderMaterial *runtime_material = blmat.get();
  m_owned_runtime_materials.push_back(std::move(blmat));
  RegisterMaterial(runtime_material, mat);
  return runtime_material;
}

KX_BlenderMaterial *BL_SceneConverter::FindMaterial(blender::Material *mat)
{
  return m_map_mesh_to_polyaterial[mat];
}

void BL_SceneConverter::AdoptLookups(BL_SceneConverter &other)
{
  /* An entry this converter has stays; the empty ones lookups leave (operator[]) are filled. */
  const auto adopt = [](auto &mine, const auto &theirs) {
    for (const auto &[key, value] : theirs) {
      if (value == nullptr) {
        continue;
      }
      auto [it, inserted] = mine.insert({key, value});
      if (!inserted && it->second == nullptr) {
        it->second = value;
      }
    }
  };
  adopt(m_map_blender_to_gameobject, other.m_map_blender_to_gameobject);
  adopt(m_map_mesh_to_gamemesh, other.m_map_mesh_to_gamemesh);
  adopt(m_map_mesh_to_polyaterial, other.m_map_mesh_to_polyaterial);
  adopt(m_map_blender_to_gameactuator, other.m_map_blender_to_gameactuator);
  adopt(m_map_blender_to_gamecontroller, other.m_map_blender_to_gamecontroller);
  for (std::unique_ptr<KX_BlenderMaterial> &material : other.m_owned_runtime_materials) {
    m_owned_runtime_materials.push_back(std::move(material));
  }
  other.m_owned_runtime_materials.clear();
}

void BL_SceneConverter::RegisterGameActuator(SCA_IActuator *act, blender::bActuator *for_actuator)
{
  m_map_blender_to_gameactuator[for_actuator] = act;
}

SCA_IActuator *BL_SceneConverter::FindGameActuator(blender::bActuator *for_actuator)
{
  return m_map_blender_to_gameactuator[for_actuator];
}

void BL_SceneConverter::RegisterGameController(SCA_IController *cont,
                                                      blender::bController *for_controller)
{
  m_map_blender_to_gamecontroller[for_controller] = cont;
}

SCA_IController *BL_SceneConverter::FindGameController(blender::bController *for_controller)
{
  return m_map_blender_to_gamecontroller[for_controller];
}

void BL_SceneConverter::AddPendingSuspendDynamics(PHY_IPhysicsController *controller)
{
  if (controller) {
    m_pending_parent_suspends.push_back(controller);
  }
}

void BL_SceneConverter::FlushPendingSuspendDynamics()
{
  for (PHY_IPhysicsController *controller : m_pending_parent_suspends) {
    controller->SuspendDynamics(false);
  }
  m_pending_parent_suspends.clear();
}

void BL_SceneConverter::AddPendingJoltTransformParenting(KX_GameObject *gameobject)
{
  if (gameobject) {
    m_pending_jolt_transform_parenting.push_back(gameobject);
  }
}

void BL_SceneConverter::FlushPendingJoltTransformParenting()
{
  for (KX_GameObject *gameobject : m_pending_jolt_transform_parenting) {
    if (!gameobject->BeginAuthoredTransformParenting()) {
      CM_Warning("Jolt transform parenting was not activated for authored parented object '"
                 << gameobject->GetName()
                 << "'; its independently simulated body and constraints were left unchanged.");
    }
  }
  m_pending_jolt_transform_parenting.clear();
}
