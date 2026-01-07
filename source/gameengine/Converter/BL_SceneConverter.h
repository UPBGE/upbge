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
namespace blender { struct Object; }
namespace blender { struct Mesh; }
namespace blender { struct Material; }
namespace blender { struct bActuator; }
namespace blender { struct bController; }

class BL_SceneConverter {
  friend BL_Converter;

 private:
  std::vector<KX_BlenderMaterial *> m_materials;
  std::vector<RAS_MeshObject *> m_meshobjects;

  std::map<blender::Object *, KX_GameObject *> m_map_blender_to_gameobject;
  std::map<blender::Mesh *, RAS_MeshObject *> m_map_mesh_to_gamemesh;
  std::map<blender::Material *, KX_BlenderMaterial *> m_map_mesh_to_polyaterial;
  std::map<blender::bActuator *, SCA_IActuator *> m_map_blender_to_gameactuator;
  std::map<blender::bController *, SCA_IController *> m_map_blender_to_gamecontroller;

 public:
  BL_SceneConverter();
  ~BL_SceneConverter();

  // Disable dangerous copy.
  BL_SceneConverter(const BL_SceneConverter &other) = delete;

  void RegisterGameObject(KX_GameObject *gameobject, blender::Object *for_blenderobject);
  void UnregisterGameObject(KX_GameObject *gameobject);
  KX_GameObject *FindGameObject(blender::Object *for_blenderobject);

  void RegisterGameMesh(RAS_MeshObject *gamemesh, blender::Mesh *for_blendermesh);
  RAS_MeshObject *FindGameMesh(blender::Mesh *for_blendermesh);

  void RegisterMaterial(KX_BlenderMaterial *blmat, blender::Material *mat);
  KX_BlenderMaterial *FindMaterial(blender::Material *mat);

  void RegisterGameActuator(SCA_IActuator *act, blender::bActuator *for_actuator);
  SCA_IActuator *FindGameActuator(blender::bActuator *for_actuator);

  void RegisterGameController(SCA_IController *cont, blender::bController *for_controller);
  SCA_IController *FindGameController(blender::bController *for_controller);
};
