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
#  include "BL_Material.h"
#  include "KX_Mesh.h"
#  include "BL_ConvertObjectInfo.h"
#  include "BL_ScalarInterpolator.h"
#  include "BL_ActionData.h"
#endif

#include "BL_SceneConverter.h"

#include "CM_Thread.h"

class EXP_StringValue;
class BL_SceneConverter;
class BL_ConvertObjectInfo;
class KX_KetsjiEngine;
class KX_LibLoadStatus;
class BL_Material;
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
		UniquePtrList<BL_Material> m_materials;
		UniquePtrList<KX_Mesh> m_meshobjects;
		UniquePtrList<BL_ActionData> m_actions;
		UniquePtrList<BL_ConvertObjectInfo> m_objectInfos;

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

	/// List of loaded libraries to merge.
	std::vector<KX_LibLoadStatus *> m_mergequeue;
	/// List of libraries to free.
	std::vector<Main *> m_freeQueue;

	/// Blender current maggie at game start.
	Main *m_maggie;
	/// Libloaded maggies.
	std::vector<Main *> m_dynamicMaggies;
	/// All maggies, original and loaded.
	std::vector<Main *> m_maggies;
	/// Loaded library status associated to library.
	std::unordered_map<Main *, std::unique_ptr<KX_LibLoadStatus> > m_libloadStatus;

	KX_KetsjiEngine *m_ketsjiEngine;
	bool m_alwaysUseExpandFraming;
	float m_camZoom;

	/// Partially convert a potential libloaded scene.
	void ConvertScene(BL_SceneConverter& converter, bool libloading, bool actions);

	/** Convert all scene data that can't in a separate thread such as python components.
	 * \param converter The scene convert to finalize.
	 */
	void PostConvertScene(const BL_SceneConverter& converter);

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

	/** Regenerate material shader after a converting or merging a scene
	 * depending on all the lights into the destination scene.
	 */
	void ReloadShaders(KX_Scene *scene);
	/// Regenerate shaders of material in given scene converter, used when creating mesh. 
	void ReloadShaders(const BL_SceneConverter& converter);

	/// Delay library merging to ProcessScheduledLibraries.
	void AddScenesToMergeQueue(KX_LibLoadStatus *status);

	/** Asynchronously convert scenes from a library.
	 * \param ptr Pointer to the library status.
	 */
	static void AsyncConvertTask(TaskPool *pool, void *ptr, int UNUSED(threadid));

	Main *GetLibraryPath(const std::string& path);

	KX_LibLoadStatus *LinkBlendFile(BlendHandle *blendlib, const char *path, char *group, KX_Scene *scene_merge, char **err_str, short options);

	/// Free blend file and remove data from merged scene.
	bool FreeBlendFileData(Main *maggie);
	/// Free blend file and remove library from internal lists.
	void FreeBlendFile(Main *maggie);

public:
	BL_Converter(Main *maggie, KX_KetsjiEngine *engine, bool alwaysUseExpandFraming, float camZoom);
	virtual ~BL_Converter();

	/// Fully convert a non-libloaded scene.
	void ConvertScene(KX_Scene *scene);

	/** This function removes all entities stored in the converter for that scene
	 * It should be used instead of direct delete scene
	 * Note that there was some provision for sharing entities (meshes...) between
	 * scenes but that is now disabled so all scene will have their own copy
	 * and we can delete them here.
	 * \param scene The scene to clean.
	 */
	void RemoveScene(KX_Scene *scene);

	/// Register a mesh object copy.
	void RegisterMesh(KX_Scene *scene, KX_Mesh *mesh);

	Scene *GetBlenderSceneForName(const std::string& name);
	EXP_ListValue<EXP_StringValue> *GetInactiveSceneNames();

	/// Return a new empty library of name path.
	Main *CreateLibrary(const std::string& path);
	bool ExistLibrary(const std::string& path) const;
	std::vector<std::string> GetLibraryNames() const;

	KX_LibLoadStatus *LinkBlendFileMemory(void *data, int length, const char *path, char *group, KX_Scene *scene_merge, char **err_str, short options);
	KX_LibLoadStatus *LinkBlendFilePath(const char *path, char *group, KX_Scene *scene_merge, char **err_str, short options);

	/// Register library to free by name.
	bool FreeBlendFile(const std::string& path);

	KX_Mesh *ConvertMeshSpecial(KX_Scene *kx_scene, Main *maggie, const std::string& name);

	/// Merge scheduled loaded libraries and remove scheduled libraries.
	void ProcessScheduledLibraries();
	/// Wait until all libraries are loaded.
	void FinalizeAsyncLoads();

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
