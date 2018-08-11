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

/** \file BL_Converter.h
 *  \ingroup bgeconv
 */

#ifndef __KX_BLENDERCONVERTER_H__
#define __KX_BLENDERCONVERTER_H__

#include <map>
#include <vector>

#ifdef _MSC_VER // MSVC doesn't support incomplete type in std::unique_ptr.
#  include "KX_BlenderMaterial.h"
#  include "KX_Mesh.h"
#  include "BL_ConvertObjectInfo.h"
#  include "BL_ScalarInterpolator.h"
#endif

#include "CM_Thread.h"

class EXP_StringValue;
class BL_SceneConverter;
class BL_ConvertObjectInfo;
class KX_KetsjiEngine;
class KX_LibLoadStatus;
class KX_BlenderMaterial;
class BL_InterpolatorList;
class SCA_IActuator;
class SCA_IController;
class KX_Mesh;
struct Main;
struct BlendHandle;
struct Mesh;
struct Scene;
struct Material;
struct bAction;
struct bActuator;
struct bController;
struct TaskPool;

template<class Value>
using UniquePtrList = std::vector<std::unique_ptr<Value> >;

class BL_Converter
{
private:
	class SceneSlot
	{
	public:
		UniquePtrList<KX_BlenderMaterial> m_materials;
		UniquePtrList<KX_Mesh> m_meshobjects;
		UniquePtrList<BL_InterpolatorList> m_interpolators;
		UniquePtrList<BL_ConvertObjectInfo> m_objectInfos;

		std::map<bAction *, BL_InterpolatorList *> m_actionToInterp;

		SceneSlot();
		SceneSlot(const BL_SceneConverter& converter);
		~SceneSlot();

		void Merge(SceneSlot& other);
		void Merge(const BL_SceneConverter& converter);
	};

	std::map<KX_Scene *, SceneSlot> m_sceneSlots;

	struct ThreadInfo {
		TaskPool *m_pool;
		CM_ThreadMutex m_mutex;
	} m_threadinfo;

	// Saved KX_LibLoadStatus objects
	std::map<std::string, KX_LibLoadStatus *> m_status_map;
	std::vector<KX_LibLoadStatus *> m_mergequeue;

	Main *m_maggie;
	std::vector<Main *> m_DynamicMaggie;

	KX_KetsjiEngine *m_ketsjiEngine;
	bool m_alwaysUseExpandFraming;
	float m_camZoom;

	/** Merge all data contained in the scene converter to the scene slot of
	 * the destination scene and update the data to use the destination scene.
	 * \param to The destination scene.
	 * \param converter The scene converted of the data to merge.
	 */
	void MergeSceneData(KX_Scene *to, const BL_SceneConverter& converter);

	/** Complete process of scene merging:
	 * - post convert
	 * - merge data
	 * - merge scene (KX_Scene::MergeScene)
	 * - finalize data
	 */
	void MergeScene(KX_Scene *to, const BL_SceneConverter& converter);

public:
	BL_Converter(Main *maggie, KX_KetsjiEngine *engine, bool alwaysUseExpandFraming, float camZoom);
	virtual ~BL_Converter();

	void ConvertScene(BL_SceneConverter& converter, bool libloading);

	/** Convert all scene data that can't in a separate thread such as python components.
	 * \param converter The scene convert to finalize.
	 */
	void PostConvertScene(const BL_SceneConverter& converter);

	/** Finalize all data depending on scene context after a potential scene merging,
	 * such as shader creation depending on lights into scene.
	 */
	void FinalizeSceneData(const BL_SceneConverter& converter);

	/** This function removes all entities stored in the converter for that scene
	 * It should be used instead of direct delete scene
	 * Note that there was some provision for sharing entities (meshes...) between
	 * scenes but that is now disabled so all scene will have their own copy
	 * and we can delete them here.
	 * \param scene The scene to clean.
	 */
	void RemoveScene(KX_Scene *scene);

	void RegisterInterpolatorList(KX_Scene *scene, BL_InterpolatorList *interpolator, bAction *for_act);
	BL_InterpolatorList *FindInterpolatorList(KX_Scene *scene, bAction *for_act);
	/// Register a mesh object copy.
	void RegisterMesh(KX_Scene *scene, KX_Mesh *mesh);

	Scene *GetBlenderSceneForName(const std::string& name);
	EXP_ListValue<EXP_StringValue> *GetInactiveSceneNames();

	Main *CreateMainDynamic(const std::string& path);
	Main *GetMainDynamicPath(const std::string& path) const;
	const std::vector<Main *> &GetMainDynamic() const;

	KX_LibLoadStatus *LinkBlendFileMemory(void *data, int length, const char *path, char *group, KX_Scene *scene_merge, char **err_str, short options);
	KX_LibLoadStatus *LinkBlendFilePath(const char *path, char *group, KX_Scene *scene_merge, char **err_str, short options);
	KX_LibLoadStatus *LinkBlendFile(BlendHandle *blendlib, const char *path, char *group, KX_Scene *scene_merge, char **err_str, short options);

	bool FreeBlendFile(Main *maggie);
	bool FreeBlendFile(const std::string& path);

	KX_Mesh *ConvertMeshSpecial(KX_Scene *kx_scene, Main *maggie, const std::string& name);

	void MergeAsyncLoads();
	void FinalizeAsyncLoads();
	void AddScenesToMergeQueue(KX_LibLoadStatus *status);

	void PrintStats();

	// LibLoad Options.
	enum
	{
		LIB_LOAD_LOAD_ACTIONS = 1,
		LIB_LOAD_VERBOSE = 2,
		LIB_LOAD_LOAD_SCRIPTS = 4,
		LIB_LOAD_ASYNC = 8,
	};
};

#endif  // __KX_BLENDERCONVERTER_H__
