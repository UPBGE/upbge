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

/** \file BL_SceneConverter.h
 *  \ingroup bgeconv
 */

#pragma once

#include <map>
#include <vector>

#include "CM_Message.h"

class SCA_IActuator;
class SCA_IController;
class RAS_MeshObject;
class KX_BlenderMaterial;
class BL_Converter;
class KX_GameObject;
struct Object;
struct Mesh;
struct Material;
struct bActuator;
struct bController;

class BL_SceneConverter {
  friend BL_Converter;

 private:
  std::vector<KX_BlenderMaterial *> m_materials;
  std::vector<RAS_MeshObject *> m_meshobjects;

  std::map<Object *, KX_GameObject *> m_map_blender_to_gameobject;
  std::map<Mesh *, RAS_MeshObject *> m_map_mesh_to_gamemesh;
  std::map<Material *, KX_BlenderMaterial *> m_map_mesh_to_polyaterial;
  std::map<bActuator *, SCA_IActuator *> m_map_blender_to_gameactuator;
  std::map<bController *, SCA_IController *> m_map_blender_to_gamecontroller;

 public:
  BL_SceneConverter();
  ~BL_SceneConverter();

  // Disable dangerous copy.
  BL_SceneConverter(const BL_SceneConverter &other) = delete;

  void RegisterGameObject(KX_GameObject *gameobject, Object *for_blenderobject);
  void UnregisterGameObject(KX_GameObject *gameobject);
  KX_GameObject *FindGameObject(Object *for_blenderobject);

  void RegisterGameMesh(RAS_MeshObject *gamemesh, Mesh *for_blendermesh);
  RAS_MeshObject *FindGameMesh(Mesh *for_blendermesh);

  void RegisterMaterial(KX_BlenderMaterial *blmat, Material *mat);
  KX_BlenderMaterial *FindMaterial(Material *mat);

  void RegisterGameActuator(SCA_IActuator *act, bActuator *for_actuator);
  SCA_IActuator *FindGameActuator(bActuator *for_actuator);

  void RegisterGameController(SCA_IController *cont, bController *for_controller);
  SCA_IController *FindGameController(bController *for_controller);
};
