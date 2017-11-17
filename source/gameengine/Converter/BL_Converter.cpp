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

/** \file gameengine/Converter/BL_Converter.cpp
 *  \ingroup bgeconv
 */

#ifdef _MSC_VER
#  pragma warning (disable:4786)  // suppress stl-MSVC debug info warning
#endif

#include "KX_Scene.h"
#include "KX_GameObject.h"
#include "KX_Mesh.h"
#include "RAS_BucketManager.h"
#include "KX_PhysicsEngineEnums.h"
#include "KX_KetsjiEngine.h"
#include "KX_PythonInit.h" // So we can handle adding new text datablocks for Python to import
#include "KX_LibLoadStatus.h"
#include "BL_ActionData.h"
#include "BL_Converter.h"
#include "BL_SceneConverter.h"
#include "BL_BlenderDataConversion.h"
#include "BL_ConvertObjectInfo.h"
#include "BL_ActionActuator.h"
#include "KX_BlenderMaterial.h"

#include "EXP_StringValue.h"

#ifdef WITH_PYTHON
#  include "Texture.h" // For FreeAllTextures.
#endif  // WITH_PYTHON

// This list includes only data type definitions
#include "DNA_scene_types.h"
#include "BKE_main.h"

extern "C" {
#  include "DNA_mesh_types.h"
#  include "DNA_material_types.h"
#  include "BLI_blenlib.h"
#  include "BLI_linklist.h"
#  include "BLO_readfile.h"
#  include "BKE_global.h"
#  include "BKE_library.h"
#  include "BKE_material.h" // BKE_material_copy
#  include "BKE_mesh.h" // BKE_mesh_copy
#  include "BKE_idcode.h"
#  include "BKE_report.h"
}

#include "BLI_task.h"
#include "CM_Message.h"

#include <cstring>
#include <memory>

BL_Converter::SceneSlot::SceneSlot() = default;

BL_Converter::SceneSlot::SceneSlot(const BL_SceneConverter& converter)
{
	Merge(converter);
}

BL_Converter::SceneSlot::~SceneSlot() = default;

void BL_Converter::SceneSlot::Merge(BL_Converter::SceneSlot& other)
{
	m_materials.insert(m_materials.begin(),
	                   std::make_move_iterator(other.m_materials.begin()),
	                   std::make_move_iterator(other.m_materials.end()));
	m_meshobjects.insert(m_meshobjects.begin(),
	                     std::make_move_iterator(other.m_meshobjects.begin()),
	                     std::make_move_iterator(other.m_meshobjects.end()));
	m_objectInfos.insert(m_objectInfos.begin(),
	                     std::make_move_iterator(other.m_objectInfos.begin()),
	                     std::make_move_iterator(other.m_objectInfos.end()));
	m_actions.insert(m_actions.begin(),
					 std::make_move_iterator(other.m_actions.begin()),
					 std::make_move_iterator(other.m_actions.end()));
}

void BL_Converter::SceneSlot::Merge(const BL_SceneConverter& converter)
{
	for (KX_BlenderMaterial *mat : converter.m_materials) {
		m_materials.emplace_back(mat);
	}
	for (KX_Mesh *meshobj : converter.m_meshobjects) {
		m_meshobjects.emplace_back(meshobj);
	}
	for (BL_ConvertObjectInfo *info : converter.m_objectInfos) {
		m_objectInfos.emplace_back(info);
	}
	for (BL_ActionData *action : converter.m_actions) {
		m_actions.emplace_back(action);
	}
}

BL_Converter::BL_Converter(Main *maggie, KX_KetsjiEngine *engine, bool alwaysUseExpandFraming, float camZoom)
	:m_maggie(maggie),
	m_ketsjiEngine(engine),
	m_alwaysUseExpandFraming(alwaysUseExpandFraming),
	m_camZoom(camZoom)
{
	BKE_main_id_tag_all(maggie, LIB_TAG_DOIT, false);  // avoid re-tagging later on
	m_threadinfo.m_pool = BLI_task_pool_create(engine->GetTaskScheduler(), nullptr);

	m_maggies.push_back(m_maggie);
}

BL_Converter::~BL_Converter()
{
	// free any data that was dynamically loaded
	while (!m_dynamicMaggies.empty()) {
		FreeBlendFile(m_dynamicMaggies.front());
	}

	/* Thread infos like mutex must be freed after FreeBlendFile function.
	   Because it needs to lock the mutex, even if there's no active task when it's
	   in the scene converter destructor. */
	BLI_task_pool_free(m_threadinfo.m_pool);
}

Scene *BL_Converter::GetBlenderSceneForName(const std::string &name)
{
	for (Main *maggie : m_maggies) {
		Scene *sce = (Scene *)BLI_findstring(&maggie->scene, name.c_str(), offsetof(ID, name) + 2);
		if (sce) {
			return sce;
		}
	}

	return nullptr;
}

EXP_ListValue<EXP_StringValue> *BL_Converter::GetInactiveSceneNames()
{
	EXP_ListValue<EXP_StringValue> *list = new EXP_ListValue<EXP_StringValue>();

	for (Scene *sce = (Scene *)m_maggie->scene.first; sce; sce = (Scene *)sce->id.next) {
		const char *name = sce->id.name + 2;
		if (m_ketsjiEngine->CurrentScenes()->FindValue(name)) {
			continue;
		}
		EXP_StringValue *item = new EXP_StringValue(name, name);
		list->Add(item);
	}

	return list;
}

void BL_Converter::ConvertScene(KX_Scene *scene)
{
	BL_SceneConverter converter(scene, BL_Resource::Library(m_maggie));
	ConvertScene(converter, false, true);
	PostConvertScene(converter);
	m_sceneSlots.emplace(scene, converter);
	ReloadShaders(scene);
}

void BL_Converter::ConvertScene(BL_SceneConverter& converter, bool libloading, bool actions)
{
	KX_Scene *scene = converter.GetScene();

	BL_ConvertBlenderObjects(
		m_maggie,
		scene,
		m_ketsjiEngine,
		m_ketsjiEngine->GetRasterizer(),
		m_ketsjiEngine->GetCanvas(),
		converter,
		m_alwaysUseExpandFraming,
		m_camZoom,
		libloading);

	// Handle actions.
	if (actions) {
		BL_ConvertActions(scene, m_maggie, converter);
	}
}

void BL_Converter::PostConvertScene(const BL_SceneConverter& converter)
{
	BL_PostConvertBlenderObjects(converter.GetScene(), converter);
}

void BL_Converter::RemoveScene(KX_Scene *scene)
{
#ifdef WITH_PYTHON
	Texture::FreeAllTextures(scene);
#endif  // WITH_PYTHON

	/* Delete the meshes as some one of them depends to the data owned by the scene
	 * e.g the display array bucket owned by the meshes and needed to be unregistered
	 * from the bucket manager in the scene.
	 */
	SceneSlot& sceneSlot = m_sceneSlots[scene];
	sceneSlot.m_meshobjects.clear();

	// Delete the scene.
	scene->Release();

	m_sceneSlots.erase(scene);
}

void BL_Converter::RegisterMesh(KX_Scene *scene, KX_Mesh *mesh)
{
	scene->GetLogicManager()->RegisterMeshName(mesh->GetName(), mesh);
	m_sceneSlots[scene].m_meshobjects.emplace_back(mesh);
}

Main *BL_Converter::CreateLibrary(const std::string& path)
{
	Main *maggie = BKE_main_new();
	strncpy(maggie->name, path.c_str(), sizeof(maggie->name) - 1);
	m_dynamicMaggies.push_back(maggie);

	return maggie;
}

bool BL_Converter::ExistLibrary(const std::string& path) const
{
	for (Main *maggie : m_dynamicMaggies) {
		if (BLI_path_cmp(maggie->name, path.c_str()) == 0) {
			return true;
		}
	}

	return false;
}

std::vector<std::string> BL_Converter::GetLibraryNames() const
{
	std::vector<std::string> names;
	for (Main *maggie : m_dynamicMaggies) {
		names.push_back(maggie->name);
	}

	return names;
}

void BL_Converter::ProcessScheduledLibraries()
{
	m_threadinfo.m_mutex.Lock();
	const std::vector<KX_LibLoadStatus *> mergeQueue = m_mergequeue;
	m_mergequeue.clear();
	m_threadinfo.m_mutex.Unlock();

	for (KX_LibLoadStatus *libload : mergeQueue) {
		KX_Scene *mergeScene = libload->GetMergeScene();
		std::vector<BL_SceneConverter>& converters = libload->GetSceneConverters();
		for (const BL_SceneConverter& converter : converters) {
			MergeScene(mergeScene, converter);
		}

		libload->Finish();
	}

	for (Main *maggie : m_freeQueue) {
		FreeBlendFileData(maggie);
	}
	m_freeQueue.clear();
}

void BL_Converter::FinalizeAsyncLoads()
{
	// Finish all loading libraries.
	BLI_task_pool_work_and_wait(m_threadinfo.m_pool);
	// Merge all libraries data in the current scene, to avoid memory leak of unmerged scenes.
	ProcessScheduledLibraries();
}

void BL_Converter::AddScenesToMergeQueue(KX_LibLoadStatus *status)
{
	m_threadinfo.m_mutex.Lock();
	m_mergequeue.push_back(status);
	m_threadinfo.m_mutex.Unlock();
}

void BL_Converter::AsyncConvertTask(TaskPool *pool, void *ptr, int UNUSED(threadid))
{
	KX_LibLoadStatus *status = static_cast<KX_LibLoadStatus *>(ptr);
	BL_Converter *converter = status->GetConverter();

	std::vector<BL_SceneConverter>& converters = status->GetSceneConverters();
	for (BL_SceneConverter& sceneConverter : converters) {
		converter->ConvertScene(sceneConverter, true, false);
		status->AddProgress((1.0f / converters.size()) * 0.9f); // We'll call conversion 90% and merging 10% for now
	}

	status->GetConverter()->AddScenesToMergeQueue(status);
}

Main *BL_Converter::GetLibraryPath(const std::string& path)
{
	for (Main *maggie : m_dynamicMaggies) {
		if (BLI_path_cmp(maggie->name, path.c_str()) == 0) {
			return maggie;
		}
	}

	return nullptr;
}

KX_LibLoadStatus *BL_Converter::LinkBlendFileMemory(void *data, int length, const char *path, char *group, KX_Scene *scene_merge, char **err_str, short options)
{
	BlendHandle *blendlib = BLO_blendhandle_from_memory(data, length);

	// Error checking is done in LinkBlendFile
	return LinkBlendFile(blendlib, path, group, scene_merge, err_str, options);
}

KX_LibLoadStatus *BL_Converter::LinkBlendFilePath(const char *filepath, char *group, KX_Scene *scene_merge, char **err_str, short options)
{
	BlendHandle *blendlib = BLO_blendhandle_from_file(filepath, nullptr);

	// Error checking is done in LinkBlendFile
	return LinkBlendFile(blendlib, filepath, group, scene_merge, err_str, options);
}

static void load_datablocks(Main *main_tmp, BlendHandle *blendlib, const char *path, int idcode)
{
	LinkNode *names = nullptr;

	int totnames_dummy;
	names = BLO_blendhandle_get_datablock_names(blendlib, idcode, &totnames_dummy);

	int i = 0;
	LinkNode *n = names;
	while (n) {
		BLO_library_link_named_part(main_tmp, &blendlib, idcode, (char *)n->link);
		n = (LinkNode *)n->next;
		i++;
	}
	BLI_linklist_free(names, free); // free linklist *and* each node's data
}

KX_LibLoadStatus *BL_Converter::LinkBlendFile(BlendHandle *blendlib, const char *path, char *group, KX_Scene *scene_merge, char **err_str, short options)
{
	const int idcode = BKE_idcode_from_name(group);
	static char err_local[255];

	// only scene and mesh supported right now
	if (!ELEM(idcode, ID_SCE, ID_ME, ID_AC)) {
		snprintf(err_local, sizeof(err_local), "invalid ID type given \"%s\"\n", group);
		*err_str = err_local;
		BLO_blendhandle_close(blendlib);
		return nullptr;
	}

	if (ExistLibrary(path)) {
		snprintf(err_local, sizeof(err_local), "blend file already open \"%s\"\n", path);
		*err_str = err_local;
		BLO_blendhandle_close(blendlib);
		return nullptr;
	}

	if (blendlib == nullptr) {
		snprintf(err_local, sizeof(err_local), "could not open blendfile \"%s\"\n", path);
		*err_str = err_local;
		return nullptr;
	}

	Main *main_newlib = BKE_main_new();

	ReportList reports;
	BKE_reports_init(&reports, RPT_STORE);

	// Created only for linking, then freed.
	Main *main_tmp = BLO_library_link_begin(main_newlib, &blendlib, path);
	load_datablocks(main_tmp, blendlib, path, idcode);

	// In case of scene, optionally link texts and actions.
	if (idcode == ID_SCE) {
		if (options & LIB_LOAD_LOAD_SCRIPTS) {
			load_datablocks(main_tmp, blendlib, path, ID_TXT);
		}
		if (options & LIB_LOAD_LOAD_ACTIONS) {
			load_datablocks(main_tmp, blendlib, path, ID_AC);
		}
	}

	// Don't need any special options.
	const short flag = 0;
	BLO_library_link_end(main_tmp, &blendlib, flag, nullptr, nullptr);
	BLO_blendhandle_close(blendlib);

	BKE_reports_clear(&reports);

	BLI_strncpy(main_newlib->name, path, sizeof(main_newlib->name));

	// Debug data to load.
	if (options & LIB_LOAD_VERBOSE) {
		if (idcode == ID_AC || (options & LIB_LOAD_LOAD_ACTIONS && idcode == ID_SCE)) {
			for (bAction *act = (bAction *)main_newlib->action.first; act; act = (bAction *)act->id.next) {
				CM_Debug("action name: " << act->id.name + 2);
			}
		}
		if (ELEM(idcode, ID_ME, ID_SCE)) {
			for (Mesh *mesh = (Mesh *)main_newlib->mesh.first; mesh; mesh = (Mesh *)mesh->id.next) {
				CM_Debug("mesh name: " << mesh->id.name + 2);
			}
		}
		if (idcode == ID_SCE) {
			for (Scene *bscene = (Scene *)main_newlib->scene.first; bscene; bscene = (Scene *)bscene->id.next) {
				CM_Debug("scene name: " << bscene->id.name + 2);
			}
		}
	}

	// Linking done.

	KX_LibLoadStatus *status = new KX_LibLoadStatus(this, m_ketsjiEngine, scene_merge, path);

	const BL_Resource::Library libraryId(main_newlib);

	switch (idcode) {
		case ID_ME:
		{
			BL_SceneConverter sceneConverter(scene_merge, libraryId);
			// Convert all new meshes into BGE meshes
			for (Mesh *mesh = (Mesh *)main_newlib->mesh.first; mesh; mesh = (Mesh *)mesh->id.next) {
				BL_ConvertMesh((Mesh *)mesh, nullptr, scene_merge, sceneConverter);
			}

			// Merge the meshes and materials in the targeted scene.
			MergeSceneData(scene_merge, sceneConverter);
			// Load shaders for new created materials.
			ReloadShaders(scene_merge);
			break;
		}
		case ID_AC:
		{
			BL_SceneConverter sceneConverter(scene_merge, libraryId);
			// Convert all actions and register.
			BL_ConvertActions(scene_merge, main_newlib, sceneConverter);
			// Merge the actions in the targeted scene.
			MergeSceneData(scene_merge, sceneConverter);
			break;
		}
		case ID_SCE:
		{
			// Merge all new linked scenes into the existing one

			if (options & LIB_LOAD_LOAD_SCRIPTS) {
#ifdef WITH_PYTHON
				// Handle any text datablocks
				addImportMain(main_newlib);
#endif
			}

			/** Actions aren't owned by scenes, to merge them in the targeted scene,
			 * a global scene converter is created and register every action, then this
			 * converter is merged into the targeted scene.
			 */
			if (options & LIB_LOAD_LOAD_ACTIONS) {
				BL_SceneConverter sceneConverter(scene_merge, libraryId);
				// Convert all actions and register.
				BL_ConvertActions(scene_merge, main_newlib, sceneConverter);
				// Merge the actions in the targeted scene.
				MergeSceneData(scene_merge, sceneConverter);
			}

			for (Scene *bscene = (Scene *)main_newlib->scene.first; bscene; bscene = (Scene *)bscene->id.next) {
				KX_Scene *scene = m_ketsjiEngine->CreateScene(bscene);

				// Schedule conversion and merge.
				if (options & LIB_LOAD_ASYNC) {
					status->AddSceneConverter(scene, libraryId);
				}
				// Or proceed direct conversion and merge.
				else {
					BL_SceneConverter sceneConverter(scene, libraryId);
					ConvertScene(sceneConverter, true, false);
					MergeScene(scene_merge, sceneConverter);
				}
			}
			break;
		}
	}

	if (options & LIB_LOAD_ASYNC) {
		BLI_task_pool_push(m_threadinfo.m_pool, AsyncConvertTask, (void *)status, false, TASK_PRIORITY_LOW);
	}
	else {
		status->Finish();
	}

	// Register new library.
	m_dynamicMaggies.push_back(main_newlib);
	m_maggies.push_back(main_newlib);

	// Register associated KX_LibLoadStatus.
	m_libloadStatus[main_newlib].reset(status);

	return status;
}

bool BL_Converter::FreeBlendFileData(Main *maggie)
{
	// Indentifier used to recognize ressources of this library.
	const BL_Resource::Library libraryId(maggie);

	KX_LibLoadStatus *status = m_libloadStatus[maggie].get();
	// If the given library is currently in loading, we do nothing.
	m_threadinfo.m_mutex.Lock();
	const bool finished = status->IsFinished();
	m_threadinfo.m_mutex.Unlock();

	if (!finished) {
		CM_Error("Library (" << maggie->name << ") is currently being loaded asynchronously, and cannot be freed until this process is done");
		return false;
	}

	// For each scene try to remove any usage of ressources from the library.
	for (KX_Scene *scene : m_ketsjiEngine->CurrentScenes()) {
		// Both list containing all the scene objects.
		std::array<EXP_ListValue<KX_GameObject> *, 2> allObjects{{scene->GetObjectList(), scene->GetInactiveList()}};

		for (EXP_ListValue<KX_GameObject> *objectList : allObjects) {
			for (KX_GameObject *gameobj : objectList) {
				BL_ConvertObjectInfo *info = gameobj->GetConvertObjectInfo();
				// Object as default camera are not linked to a blender resource.
				if (!info) {
					continue;
				}

				// Free object directly depending on blender object of the library.
				if (info->Belong(libraryId)) {
					scene->DelayedRemoveObject(gameobj);
				}
				// Else try to remove used ressource (e.g actions, meshes, materials...).
				else {
					gameobj->RemoveRessources(libraryId);
				}
			}
		}

		scene->RemoveEuthanasyObjects();
	}

	// Free ressources belonging to the library and unregister them.
	for (auto& pair : m_sceneSlots) {
		KX_Scene *scene = pair.first;
		SCA_LogicManager *logicmgr = scene->GetLogicManager();
		SceneSlot& sceneSlot = pair.second;

		// Free meshes.
		for (UniquePtrList<KX_Mesh>::iterator it =  sceneSlot.m_meshobjects.begin(); it !=  sceneSlot.m_meshobjects.end(); ) {
			KX_Mesh *mesh = it->get();
			if (mesh->Belong(libraryId)) {
				logicmgr->UnregisterMesh(mesh);
				it = sceneSlot.m_meshobjects.erase(it);
			}
			else {
				++it;
			}
		}

		// Free materials.
		for (UniquePtrList<KX_BlenderMaterial>::iterator it = sceneSlot.m_materials.begin(); it != sceneSlot.m_materials.end(); ) {
			KX_BlenderMaterial *mat = it->get();
			if (mat->Belong(libraryId)) {
				scene->GetBucketManager()->RemoveMaterial(mat);
				it = sceneSlot.m_materials.erase(it);
			}
			else {
				++it;
			}
		}

		// Free actions.
		for (UniquePtrList<BL_ActionData>::iterator it = sceneSlot.m_actions.begin(); it != sceneSlot.m_actions.end(); ) {
			BL_ActionData *act = it->get();
			if (act->Belong(libraryId)) {
				logicmgr->UnregisterAction(act);
				it = sceneSlot.m_actions.erase(it);
			}
			else {
				++it;
			}
		}

		// Free object infos.
		for (UniquePtrList<BL_ConvertObjectInfo>::iterator it = sceneSlot.m_objectInfos.begin(); it != sceneSlot.m_objectInfos.end(); ) {
			BL_ConvertObjectInfo *info = it->get();
			if (info->Belong(libraryId)) {
				it = sceneSlot.m_objectInfos.erase(it);
			}
			else {
				++it;
			}
		}

		// Reload materials cause they used lamps removed now.
		scene->GetBucketManager()->ReloadMaterials();
	}

	// Remove and destruct the KX_LibLoadStatus associated to the just free library.
	m_libloadStatus.erase(maggie);

	// Actual free of the blender library.
	FreeBlendFile(maggie);

	return true;
}

void BL_Converter::FreeBlendFile(Main *maggie)
{
#ifdef WITH_PYTHON
	/* make sure this maggie is removed from the import list if it's there
	 * (this operation is safe if it isn't in the list) */
	removeImportMain(maggie);
#endif

	// Remove the library from lists.
	CM_ListRemoveIfFound(m_maggies, maggie);
	CM_ListRemoveIfFound(m_dynamicMaggies, maggie);

	BKE_main_free(maggie);
}

bool BL_Converter::FreeBlendFile(const std::string& path)
{
	Main *maggie = GetLibraryPath(path);
	if (!maggie) {
		return false;
	}

	// Delay library free in ProcessScheduledLibraries.
	m_freeQueue.push_back(maggie);
	return true;
}

void BL_Converter::MergeSceneData(KX_Scene *to, const BL_SceneConverter& converter)
{
	for (KX_Mesh *mesh : converter.m_meshobjects) {
		mesh->ReplaceScene(to);
	}

	// Do this after lights are available (scene merged) so materials can use the lights in shaders.
	for (KX_BlenderMaterial *mat : converter.m_materials) {
		mat->ReplaceScene(to);
	}

	m_sceneSlots[to].Merge(converter);
}

void BL_Converter::MergeScene(KX_Scene *to, const BL_SceneConverter& converter)
{
	PostConvertScene(converter);

	MergeSceneData(to, converter);

	KX_Scene *from = converter.GetScene();
	to->MergeScene(from);

	ReloadShaders(to);

	delete from;
}

void BL_Converter::ReloadShaders(KX_Scene *scene)
{
	for (std::unique_ptr<KX_BlenderMaterial>& mat : m_sceneSlots[scene].m_materials) {
		mat->ReloadMaterial();
	}

	KX_WorldInfo *world = scene->GetWorldInfo();
	if (world) {
		world->ReloadMaterial();
	}
}

void BL_Converter::ReloadShaders(const BL_SceneConverter& converter)
{
	for (KX_BlenderMaterial *mat : converter.m_materials) {
		mat->ReloadMaterial();
	}
}

/** This function merges a mesh from the current scene into another main
 * it does not convert */
KX_Mesh *BL_Converter::ConvertMeshSpecial(KX_Scene *kx_scene, Main *maggie, const std::string& name)
{
	Main *from_maggie;
	ID *me = nullptr;
	for (Main *main : m_maggies) {
		me = static_cast<ID *>(BLI_findstring(&main->mesh, name.c_str(), offsetof(ID, name) + 2));
		if (me) {
			from_maggie = main;
			break;
		}
	}

	if (me == nullptr) {
		CM_Error("could not be found \"" << name << "\"");
		return nullptr;
	}

	// Watch this!, if its used in the original scene can cause big troubles
	if (me->us > 0) {
#ifdef DEBUG
		CM_Debug("mesh has a user \"" << name << "\"");
#endif  // DEBUG
		me = (ID *)BKE_mesh_copy(from_maggie, (Mesh *)me);
		id_us_min(me);
	}
	BLI_remlink(&from_maggie->mesh, me); // even if we made the copy it needs to be removed
	BLI_addtail(&maggie->mesh, me);

	// Must copy the materials this uses else we cant free them
	{
		Mesh *mesh = (Mesh *)me;

		// ensure all materials are tagged
		for (int i = 0; i < mesh->totcol; i++) {
			if (mesh->mat[i]) {
				mesh->mat[i]->id.tag &= ~LIB_TAG_DOIT;
			}
		}

		for (int i = 0; i < mesh->totcol; i++) {
			Material *mat_old = mesh->mat[i];

			// if its tagged its a replaced material
			if (mat_old && (mat_old->id.tag & LIB_TAG_DOIT) == 0) {
				Material *mat_new = BKE_material_copy(from_maggie, mat_old);

				mat_new->id.tag |= LIB_TAG_DOIT;
				id_us_min(&mat_old->id);

				BLI_remlink(&from_maggie->mat, mat_new); // BKE_material_copy uses G.main, and there is no BKE_material_copy_ex
				BLI_addtail(&maggie->mat, mat_new);

				mesh->mat[i] = mat_new;

				// the same material may be used twice
				for (int j = i + 1; j < mesh->totcol; j++) {
					if (mesh->mat[j] == mat_old) {
						mesh->mat[j] = mat_new;
						id_us_plus(&mat_new->id);
						id_us_min(&mat_old->id);
					}
				}
			}
		}
	}

	BL_SceneConverter sceneConverter(kx_scene, BL_Resource::Library(maggie));

	KX_Mesh *meshobj = BL_ConvertMesh((Mesh *)me, nullptr, kx_scene, sceneConverter);

	MergeSceneData(kx_scene, sceneConverter);
	ReloadShaders(sceneConverter);

	return meshobj;
}

void BL_Converter::PrintStats()
{
	CM_Message("BGE STATS");
	CM_Message(std::endl << "Assets:");

	unsigned int nummat = 0;
	unsigned int nummesh = 0;
	unsigned int numacts = 0;

	for (const auto& pair : m_sceneSlots) {
		KX_Scene *scene = pair.first;
		const SceneSlot& sceneSlot = pair.second;

		nummat += sceneSlot.m_materials.size();
		nummesh += sceneSlot.m_meshobjects.size();
		numacts += sceneSlot.m_actions.size();

		CM_Message("\tscene: " << scene->GetName())
		CM_Message("\t\t materials: " << sceneSlot.m_materials.size());
		CM_Message("\t\t meshes: " << sceneSlot.m_meshobjects.size());
		CM_Message("\t\t actions: " << sceneSlot.m_actions.size());
	}

	CM_Message(std::endl << "Total:");
	CM_Message("\t scenes: " << m_sceneSlots.size());
	CM_Message("\t materials: " << nummat);
	CM_Message("\t meshes: " << nummesh);
	CM_Message("\t actions: " << numacts);
}
