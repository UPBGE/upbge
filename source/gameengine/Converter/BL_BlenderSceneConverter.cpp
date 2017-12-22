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

/** \file gameengine/Converter/BL_BlenderSceneConverter.cpp
 *  \ingroup bgeconv
 */

#include "BL_BlenderSceneConverter.h"
#include "BL_ConvertObjectInfo.h"
#include "KX_GameObject.h"

BL_BlenderSceneConverter::BL_BlenderSceneConverter(KX_Scene *scene)
	:m_scene(scene)
{
}

BL_BlenderSceneConverter::BL_BlenderSceneConverter(BL_BlenderSceneConverter&& other)
	:m_scene(other.m_scene),
	m_materials(std::move(other.m_materials)),
	m_meshobjects(std::move(other.m_meshobjects)),
	m_objectInfos(std::move(other.m_objectInfos)),
	m_blenderToObjectInfos(std::move(other.m_blenderToObjectInfos)),
	m_map_blender_to_gameobject(std::move(other.m_map_blender_to_gameobject)),
	m_map_mesh_to_gamemesh(std::move(other.m_map_mesh_to_gamemesh)),
	m_map_mesh_to_polyaterial(std::move(other.m_map_mesh_to_polyaterial)),
	m_map_blender_to_gameactuator(std::move(other.m_map_blender_to_gameactuator)),
	m_map_blender_to_gamecontroller(std::move(other.m_map_blender_to_gamecontroller))
{
}

KX_Scene *BL_BlenderSceneConverter::GetScene() const
{
	return m_scene;
}

void BL_BlenderSceneConverter::RegisterGameObject(KX_GameObject *gameobject, Object *for_blenderobject)
{
	// only maintained while converting, freed during game runtime
	m_map_blender_to_gameobject[for_blenderobject] = gameobject;
}

/** only need to run this during conversion since
 * m_map_blender_to_gameobject is freed after conversion */
void BL_BlenderSceneConverter::UnregisterGameObject(KX_GameObject *gameobject)
{
	Object *bobp = gameobject->GetBlenderObject();
	if (bobp) {
		std::map<Object *, KX_GameObject *>::iterator it = m_map_blender_to_gameobject.find(bobp);
		if (it->second == gameobject) {
			// also maintain m_map_blender_to_gameobject if the gameobject
			// being removed is matching the blender object
			m_map_blender_to_gameobject.erase(it);
		}
	}
}

KX_GameObject *BL_BlenderSceneConverter::FindGameObject(Object *for_blenderobject)
{
	return m_map_blender_to_gameobject[for_blenderobject];
}

void BL_BlenderSceneConverter::RegisterGameMesh(KX_Mesh *gamemesh, Mesh *for_blendermesh)
{
	if (for_blendermesh) { // dynamically loaded meshes we don't want to keep lookups for
		m_map_mesh_to_gamemesh[for_blendermesh] = gamemesh;
	}
	m_meshobjects.push_back(gamemesh);
}

KX_Mesh *BL_BlenderSceneConverter::FindGameMesh(Mesh *for_blendermesh)
{
	return m_map_mesh_to_gamemesh[for_blendermesh];
}

void BL_BlenderSceneConverter::RegisterMaterial(KX_BlenderMaterial *blmat, Material *mat)
{
	if (mat) {
		m_map_mesh_to_polyaterial[mat] = blmat;
	}
	m_materials.push_back(blmat);
}

KX_BlenderMaterial *BL_BlenderSceneConverter::FindMaterial(Material *mat)
{
	return m_map_mesh_to_polyaterial[mat];
}

void BL_BlenderSceneConverter::RegisterGameActuator(SCA_IActuator *act, bActuator *for_actuator)
{
	m_map_blender_to_gameactuator[for_actuator] = act;
}

SCA_IActuator *BL_BlenderSceneConverter::FindGameActuator(bActuator *for_actuator)
{
	return m_map_blender_to_gameactuator[for_actuator];
}

void BL_BlenderSceneConverter::RegisterGameController(SCA_IController *cont, bController *for_controller)
{
	m_map_blender_to_gamecontroller[for_controller] = cont;
}

SCA_IController *BL_BlenderSceneConverter::FindGameController(bController *for_controller)
{
	return m_map_blender_to_gamecontroller[for_controller];
}

BL_ConvertObjectInfo *BL_BlenderSceneConverter::GetObjectInfo(Object *blenderobj)
{
	const auto& it = m_blenderToObjectInfos.find(blenderobj);
	if (it == m_blenderToObjectInfos.end()) {
		BL_ConvertObjectInfo *info = m_blenderToObjectInfos[blenderobj] = new BL_ConvertObjectInfo{blenderobj, {}};
		m_objectInfos.push_back(info);
		return info;
	}

	return it->second;
}
