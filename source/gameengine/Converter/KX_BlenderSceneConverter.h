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

/** \file KX_BlenderSceneConverter.h
 *  \ingroup bgeconv
 */

#ifndef __KX_BLENDERSCENECONVERTER_H__
#define __KX_BLENDERSCENECONVERTER_H__

#include <map>
#include <vector>

class SCA_IActuator;
class SCA_IController;
class RAS_MeshObject;
class RAS_IPolyMaterial;
class KX_BlenderConverter;
class KX_GameObject;
struct Object;
struct Mesh;
struct Material;
struct bActuator;
struct bController;

class KX_BlenderSceneConverter
{
	friend KX_BlenderConverter;

private:
	std::vector<RAS_IPolyMaterial *> m_polymaterials;
	std::vector<RAS_MeshObject *> m_meshobjects;

	std::map<Object *, KX_GameObject *> m_map_blender_to_gameobject;
	std::map<Mesh *, RAS_MeshObject *> m_map_mesh_to_gamemesh;
	std::map<Material *, RAS_IPolyMaterial *> m_map_mesh_to_polyaterial;
	std::map<bActuator *, SCA_IActuator *> m_map_blender_to_gameactuator;
	std::map<bController *, SCA_IController *> m_map_blender_to_gamecontroller;

public:
	KX_BlenderSceneConverter() = default;
	~KX_BlenderSceneConverter() = default;

	// Disable dangerous copy.
	KX_BlenderSceneConverter(const KX_BlenderSceneConverter& other) = delete;

	void RegisterGameObject(KX_GameObject *gameobject, Object *for_blenderobject);
	void UnregisterGameObject(KX_GameObject *gameobject);
	KX_GameObject *FindGameObject(Object *for_blenderobject);

	void RegisterGameMesh(RAS_MeshObject *gamemesh, Mesh *for_blendermesh);
	RAS_MeshObject *FindGameMesh(Mesh *for_blendermesh);

	void RegisterPolyMaterial(RAS_IPolyMaterial *polymat, Material *mat);
	RAS_IPolyMaterial *FindPolyMaterial(Material *mat);

	void RegisterGameActuator(SCA_IActuator *act, bActuator *for_actuator);
	SCA_IActuator *FindGameActuator(bActuator *for_actuator);

	void RegisterGameController(SCA_IController *cont, bController *for_controller);
	SCA_IController *FindGameController(bController *for_controller);

	Scene *GetBlenderSceneForName(const std::string& name);
	virtual CListValue *GetInactiveSceneNames();

	Main *GetMainDynamicPath(const char *path);
	std::vector<Main *> &GetMainDynamic();

	KX_LibLoadStatus *LinkBlendFileMemory(void *data, int length, const char *path, char *group, KX_Scene *scene_merge, char **err_str, short options);
	KX_LibLoadStatus *LinkBlendFilePath(const char *path, char *group, KX_Scene *scene_merge, char **err_str, short options);
	KX_LibLoadStatus *LinkBlendFile(BlendHandle *bpy_openlib, const char *path, char *group, KX_Scene *scene_merge, char **err_str, short options);
	bool MergeScene(KX_Scene *to, KX_Scene *from);
	RAS_MeshObject *ConvertMeshSpecial(KX_Scene *kx_scene, Main *maggie, const char *name);
	bool FreeBlendFile(Main *maggie);
	bool FreeBlendFile(const char *path);

	virtual void MergeAsyncLoads();
	virtual void FinalizeAsyncLoads();
	void AddScenesToMergeQueue(KX_LibLoadStatus *status);

	void PrintStats()
	{
		CM_Message("BGE STATS!");
		CM_Message(std::endl << "Assets...");
		CM_Message("\t m_polymaterials: " << (int)m_polymaterials.size());
		CM_Message("\t m_meshobjects: " << (int)m_meshobjects.size());
		CM_Message(std::endl << "Mappings...");
		CM_Message("\t m_map_blender_to_gameobject: " << (int)m_map_blender_to_gameobject.size());
		CM_Message("\t m_map_mesh_to_gamemesh: " << (int)m_map_mesh_to_gamemesh.size());
		CM_Message("\t m_map_blender_to_gameactuator: " << (int)m_map_blender_to_gameactuator.size());
		CM_Message("\t m_map_blender_to_gamecontroller: " << (int)m_map_blender_to_gamecontroller.size());
		CM_Message("\t m_map_blender_to_gameAdtList: " << (int)m_map_blender_to_gameAdtList.size());

#ifdef WITH_CXX_GUARDEDALLOC
		MEM_printmemlist_pydict();
#endif
	}

	/* LibLoad Options */
	enum
	{
		LIB_LOAD_LOAD_ACTIONS = 1,
		LIB_LOAD_VERBOSE = 2,
		LIB_LOAD_LOAD_SCRIPTS = 4,
		LIB_LOAD_ASYNC = 8,
	};



#ifdef WITH_PYTHON
	PyObject *GetPyNamespace();
#endif

#ifdef WITH_CXX_GUARDEDALLOC
	MEM_CXX_CLASS_ALLOC_FUNCS("GE:KX_BlenderSceneConverter")
#endif
};

#endif  // __KX_BLENDERSCENECONVERTER_H__
