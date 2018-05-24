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
#include "BL_ConvertObjectInfo.h"
#include "BL_ActionData.h"
#include "KX_GameObject.h"
#include "KX_Mesh.h"
#include "KX_BlenderMaterial.h"

#include "CM_List.h"
#include "CM_Map.h"

BL_SceneConverter::BL_SceneConverter(KX_Scene *scene, const BL_Resource::Library& libraryId)
	:m_scene(scene),
	m_libraryId(libraryId)
{
}

BL_SceneConverter::BL_SceneConverter(BL_SceneConverter&& other)
	:m_scene(other.m_scene),
	m_libraryId(other.m_libraryId),
	m_materials(std::move(other.m_materials)),
	m_meshes(std::move(other.m_meshes)),
	m_objectInfos(std::move(other.m_objectInfos)),
	m_actions(std::move(other.m_actions)),
	m_objects(std::move(other.m_objects)),
	m_blenderToObjectInfos(std::move(other.m_blenderToObjectInfos)),
	m_map_blender_to_gameobject(std::move(other.m_map_blender_to_gameobject)),
	m_map_mesh_to_gamemesh(std::move(other.m_map_mesh_to_gamemesh)),
	m_map_mesh_to_polyaterial(std::move(other.m_map_mesh_to_polyaterial))
{
}

KX_Scene *BL_SceneConverter::GetScene() const
{
	return m_scene;
}

void BL_SceneConverter::RegisterObject(KX_GameObject *gameobject, Object *for_blenderobject)
{
	// only maintained while converting, freed during game runtime
	m_map_blender_to_gameobject[for_blenderobject] = gameobject;
	m_objects.push_back(gameobject);
}

/** only need to run this during conversion since
 * m_map_blender_to_gameobject is freed after conversion */
void BL_SceneConverter::UnregisterObject(KX_GameObject *gameobject)
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

	CM_ListRemoveIfFound(m_objects, gameobject);
}

KX_GameObject *BL_SceneConverter::FindObject(Object *for_blenderobject) const
{
	return CM_MapGetItemNoInsert(m_map_blender_to_gameobject, for_blenderobject);
}

void BL_SceneConverter::RegisterMesh(KX_Mesh *gamemesh, Mesh *for_blendermesh)
{
	gamemesh->SetOwner(m_libraryId);

	if (for_blendermesh) { // dynamically loaded meshes we don't want to keep lookups for
		m_map_mesh_to_gamemesh[for_blendermesh] = gamemesh;
	}
	m_meshes.push_back(gamemesh);
}

KX_Mesh *BL_SceneConverter::FindMesh(Mesh *for_blendermesh) const
{
	return CM_MapGetItemNoInsert(m_map_mesh_to_gamemesh, for_blendermesh);
}

void BL_SceneConverter::RegisterMaterial(KX_BlenderMaterial *blmat, Material *mat)
{
	blmat->SetOwner(m_libraryId);

	if (mat) {
		m_map_mesh_to_polyaterial[mat] = blmat;
	}
	m_materials.push_back(blmat);
}

KX_BlenderMaterial *BL_SceneConverter::FindMaterial(Material *mat) const
{
	return CM_MapGetItemNoInsert(m_map_mesh_to_polyaterial, mat);
}

void BL_SceneConverter::RegisterActionData(BL_ActionData *data)
{
	data->SetOwner(m_libraryId);
	m_actions.push_back(data);
}

BL_ConvertObjectInfo *BL_SceneConverter::GetObjectInfo(Object *blenderobj)
{
	const auto& it = m_blenderToObjectInfos.find(blenderobj);
	if (it == m_blenderToObjectInfos.end()) {
		BL_ConvertObjectInfo *info = m_blenderToObjectInfos[blenderobj] = new BL_ConvertObjectInfo(blenderobj);
		info->SetOwner(m_libraryId);

		m_objectInfos.push_back(info);
		return info;
	}

	return it->second;
}

const std::vector<KX_GameObject *> &BL_SceneConverter::GetObjects() const
{
	return m_objects;
}

const std::vector<KX_BlenderMaterial *> &BL_SceneConverter::GetMaterials() const
{
	return m_materials;
}
