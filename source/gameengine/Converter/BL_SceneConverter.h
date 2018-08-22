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

#ifndef __KX_BLENDERSCENECONVERTER_H__
#define __KX_BLENDERSCENECONVERTER_H__

#include "BL_Resource.h"

#include <map>
#include <vector>

class SCA_IActuator;
class SCA_IController;
class KX_Mesh;
class BL_Material;
class BL_Converter;
class BL_ConvertObjectInfo;
class BL_ActionData;
class KX_GameObject;
class KX_Scene;
class KX_LibLoadStatus;
struct Main;
struct BlendHandle;
struct Object;
struct Scene;
struct Mesh;
struct Material;
struct bAction;
struct bActuator;
struct bController;

class BL_SceneConverter
{
	friend BL_Converter;

private:
	KX_Scene *m_scene;
	const BL_Resource::Library m_libraryId;

	/// Ressources from the scene.

	std::vector<BL_Material *> m_materials;
	std::vector<KX_Mesh *> m_meshobjects;
	std::vector<BL_ConvertObjectInfo *> m_objectInfos;
	std::vector<BL_ActionData *> m_actions;

	/// List of all object converted, active and inactive, not considered as a ressource.
	std::vector<KX_GameObject *> m_objects;

	std::map<Object *, BL_ConvertObjectInfo *> m_blenderToObjectInfos;
	std::map<Object *, KX_GameObject *> m_map_blender_to_gameobject;
	std::map<Mesh *, KX_Mesh *> m_map_mesh_to_gamemesh;
	std::map<Material *, BL_Material *> m_map_mesh_to_polyaterial;
	std::map<bActuator *, SCA_IActuator *> m_map_blender_to_gameactuator;
	std::map<bController *, SCA_IController *> m_map_blender_to_gamecontroller;

public:
	BL_SceneConverter(KX_Scene *scene, const BL_Resource::Library& libraryId);
	~BL_SceneConverter() = default;

	// Disable dangerous copy.
	BL_SceneConverter(const BL_SceneConverter& other) = delete;
	BL_SceneConverter(BL_SceneConverter&& other);

	KX_Scene *GetScene() const;

	void RegisterGameObject(KX_GameObject *gameobject, Object *for_blenderobject);
	void UnregisterGameObject(KX_GameObject *gameobject);
	KX_GameObject *FindGameObject(Object *for_blenderobject) const;

	void RegisterGameMesh(KX_Mesh *gamemesh, Mesh *for_blendermesh);
	KX_Mesh *FindGameMesh(Mesh *for_blendermesh) const;

	void RegisterMaterial(BL_Material *blmat, Material *mat);
	BL_Material *FindMaterial(Material *mat) const;

	void RegisterActionData(BL_ActionData *data);

	void RegisterGameActuator(SCA_IActuator *act, bActuator *for_actuator);
	SCA_IActuator *FindGameActuator(bActuator *for_actuator) const;

	void RegisterGameController(SCA_IController *cont, bController *for_controller);
	SCA_IController *FindGameController(bController *for_controller) const;

	BL_ConvertObjectInfo *GetObjectInfo(Object *blenderobj);

	const std::vector<KX_GameObject *>& GetObjects() const;
	const std::vector<BL_Material *>& GetMaterials() const;
};

#endif  // __KX_BLENDERSCENECONVERTER_H__
