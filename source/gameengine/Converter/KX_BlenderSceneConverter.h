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

#include <stdio.h>

#include "KX_ISceneConverter.h"
#include "KX_IpoConvert.h"

#include "CM_Message.h"

#include <map>
#include <vector>

class KX_WorldInfo;
class KX_KetsjiEngine;
class KX_LibLoadStatus;
class SCA_IActuator;
class SCA_IController;
class RAS_MeshObject;
class RAS_IPolyMaterial;
class RAS_Rasterizer;
class BL_InterpolatorList;
class BL_Material;
struct Main;
struct BlendHandle;
struct Mesh;
struct Scene;
struct ThreadInfo;
struct Material;
struct bAction;
struct bActuator;
struct bController;

typedef std::map<KX_Scene *, std::map<Material *, RAS_IPolyMaterial *> > PolyMaterialCache;

class KX_BlenderSceneConverter : public KX_ISceneConverter
{
	std::map<KX_Scene *, std::vector<RAS_IPolyMaterial *> > m_polymaterials;
	std::map<KX_Scene *, std::vector<RAS_MeshObject *> > m_meshobjects;

	std::vector<KX_LibLoadStatus *> m_mergequeue;
	ThreadInfo  *m_threadinfo;

	// Cached material conversions
	PolyMaterialCache m_polymat_cache;

	// Saved KX_LibLoadStatus objects
	std::map<char *, KX_LibLoadStatus *> m_status_map;

	std::map<Object *, KX_GameObject *> m_map_blender_to_gameobject;        /* cleared after conversion */
	std::map<Mesh *, RAS_MeshObject *> m_map_mesh_to_gamemesh;              /* cleared after conversion */
	std::map<bActuator *, SCA_IActuator *> m_map_blender_to_gameactuator;       /* cleared after conversion */
	std::map<bController *, SCA_IController *> m_map_blender_to_gamecontroller; /* cleared after conversion */

	std::map<bAction *, BL_InterpolatorList *> m_map_blender_to_gameAdtList;

	Main *m_maggie;
	std::vector<Main *>   m_DynamicMaggie;

	std::string m_newfilename;
	KX_KetsjiEngine *m_ketsjiEngine;
	bool m_alwaysUseExpandFraming;

public:
	KX_BlenderSceneConverter(Main *maggie, KX_KetsjiEngine *engine);

	virtual ~KX_BlenderSceneConverter();

	/** \param Scenename name of the scene to be converted.
	 * \param destinationscene pass an empty scene, everything goes into this
	 * \param dictobj python dictionary (for pythoncontrollers)
	 */
	virtual void ConvertScene(KX_Scene *destinationscene, RAS_Rasterizer *rendertools, RAS_ICanvas *canvas, bool libloading = false);
	virtual void RemoveScene(KX_Scene *scene);

	void SetNewFileName(const std::string& filename);
	bool TryAndLoadNewFile();

	void SetAlwaysUseExpandFraming(bool to_what);

	void RegisterGameObject(KX_GameObject *gameobject, Object *for_blenderobject);
	void UnregisterGameObject(KX_GameObject *gameobject);
	KX_GameObject *FindGameObject(Object *for_blenderobject);

	void RegisterGameMesh(KX_Scene *scene, RAS_MeshObject *gamemesh, Mesh *for_blendermesh);
	RAS_MeshObject *FindGameMesh(Mesh *for_blendermesh /*, unsigned int onlayer*/);

	void RegisterPolyMaterial(KX_Scene *scene, RAS_IPolyMaterial *polymat);
	void CachePolyMaterial(KX_Scene *scene, Material *mat, RAS_IPolyMaterial *polymat);
	RAS_IPolyMaterial *FindCachedPolyMaterial(KX_Scene *scene, Material *mat);

	void RegisterInterpolatorList(BL_InterpolatorList *actList, bAction *for_act);
	BL_InterpolatorList *FindInterpolatorList(bAction *for_act);

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
};

#endif  // __KX_BLENDERSCENECONVERTER_H__
