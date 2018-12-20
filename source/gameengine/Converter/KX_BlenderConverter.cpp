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

/** \file gameengine/Converter/KX_BlenderConverter.cpp
 *  \ingroup bgeconv
 */

#ifdef _MSC_VER
#  pragma warning (disable:4786)  // suppress stl-MSVC debug info warning
#endif

#include "KX_Scene.h"
#include "KX_GameObject.h"
#include "KX_WorldInfo.h"
#include "RAS_MeshObject.h"
#include "RAS_BucketManager.h"
#include "KX_PhysicsEngineEnums.h"
#include "KX_KetsjiEngine.h"
#include "KX_PythonInit.h" // So we can handle adding new text datablocks for Python to import
#include "KX_LibLoadStatus.h"
#include "KX_BlenderScalarInterpolator.h"
#include "KX_BlenderConverter.h"
#include "KX_BlenderSceneConverter.h"
#include "BL_BlenderDataConversion.h"
#include "BL_ActionActuator.h"
#include "KX_BlenderMaterial.h"

#include "LA_SystemCommandLine.h"

#include "DummyPhysicsEnvironment.h"

#ifdef WITH_BULLET
#  include "CcdPhysicsEnvironment.h"
#endif

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
#  include "BKE_layer.h"
#  include "BKE_library.h"
#  include "BKE_material.h" // BKE_material_copy
#  include "BKE_mesh.h" // BKE_mesh_copy
#  include "BKE_idcode.h"
#  include "BKE_report.h"
#  include "BKE_scene.h"
}

#include "BLI_task.h"
#include "CM_Message.h"

KX_BlenderConverter::SceneSlot::SceneSlot() = default;

KX_BlenderConverter::SceneSlot::SceneSlot(const KX_BlenderSceneConverter& converter)
{
	Merge(converter);
}

KX_BlenderConverter::SceneSlot::~SceneSlot() = default;

void KX_BlenderConverter::SceneSlot::Merge(KX_BlenderConverter::SceneSlot& other)
{
	m_interpolators.insert(m_interpolators.begin(),
						   std::make_move_iterator(other.m_interpolators.begin()),
						   std::make_move_iterator(other.m_interpolators.end()));
	m_materials.insert(m_materials.begin(),
						   std::make_move_iterator(other.m_materials.begin()),
						   std::make_move_iterator(other.m_materials.end()));
	m_meshobjects.insert(m_meshobjects.begin(),
						 std::make_move_iterator(other.m_meshobjects.begin()),
						 std::make_move_iterator(other.m_meshobjects.end()));
	m_actionToInterp.insert(other.m_actionToInterp.begin(), other.m_actionToInterp.end());
}

void KX_BlenderConverter::SceneSlot::Merge(const KX_BlenderSceneConverter& converter)
{
	for (KX_BlenderMaterial *mat : converter.m_materials) {
		m_materials.emplace_back(mat);
	}
	for (RAS_MeshObject *meshobj : converter.m_meshobjects) {
		m_meshobjects.emplace_back(meshobj);
	}
}

KX_BlenderConverter::KX_BlenderConverter(Main *maggie, KX_KetsjiEngine *engine)
	:m_maggie(maggie),
	m_ketsjiEngine(engine),
	m_alwaysUseExpandFraming(false)
{
	BKE_main_id_tag_all(maggie, LIB_TAG_DOIT, false);  // avoid re-tagging later on
	m_threadinfo.m_pool = BLI_task_pool_create(engine->GetTaskScheduler(), nullptr);
}

KX_BlenderConverter::~KX_BlenderConverter()
{
	// free any data that was dynamically loaded
	while (m_DynamicMaggie.size() != 0) {
		FreeBlendFile(m_DynamicMaggie[0]);
	}

	m_DynamicMaggie.clear();

	/* Thread infos like mutex must be freed after FreeBlendFile function.
	   Because it needs to lock the mutex, even if there's no active task when it's
	   in the scene converter destructor. */
	BLI_task_pool_free(m_threadinfo.m_pool);
}

Main *KX_BlenderConverter::GetMain()
{
	return m_maggie;
}

Scene *KX_BlenderConverter::GetBlenderSceneForName(const std::string &name)
{
	Scene *sce;

	// Find the specified scene by name, or nullptr if nothing matches.
	if ((sce = (Scene *)BLI_findstring(&m_maggie->scene, name.c_str(), offsetof(ID, name) + 2))) {
		return sce;
	}

	for (Main *main : m_DynamicMaggie) {
		if ((sce = (Scene *)BLI_findstring(&main->scene, name.c_str(), offsetof(ID, name) + 2))) {
			return sce;
		}
	}

	return nullptr;
}

CListValue<CStringValue> *KX_BlenderConverter::GetInactiveSceneNames()
{
	CListValue<CStringValue> *list = new CListValue<CStringValue>();

	for (Scene *sce = (Scene *)m_maggie->scene.first; sce; sce = (Scene *)sce->id.next) {
		const char *name = sce->id.name + 2;
		if (m_ketsjiEngine->CurrentScenes()->FindValue(name)) {
			continue;
		}
		CStringValue *item = new CStringValue(name, name);
		list->Add(item);
	}

	return list;
}

void KX_BlenderConverter::ConvertScene(KX_Scene *destinationscene, RAS_Rasterizer *rasty, RAS_ICanvas *canvas, bool libloading)
{

	// Find out which physics engine
	Scene *blenderscene = destinationscene->GetBlenderScene();

	PHY_IPhysicsEnvironment *phy_env = nullptr;

	e_PhysicsEngine physics_engine = UseBullet;

	// This doesn't really seem to do anything except cause potential issues
	// when doing threaded conversion, so it's disabled for now.
	// SG_SetActiveStage(SG_STAGE_CONVERTER);

	switch (blenderscene->gm.physicsEngine)
	{
#ifdef WITH_BULLET
		case WOPHY_BULLET:
		{
			SYS_SystemHandle syshandle = SYS_GetSystem(); /*unused*/
			int visualizePhysics = SYS_GetCommandLineInt(syshandle, "show_physics", 0);

			phy_env = CcdPhysicsEnvironment::Create(blenderscene, visualizePhysics);
			physics_engine = UseBullet;
			break;
		}
#endif
		default:
		case WOPHY_NONE:
		{
			// We should probably use some sort of factory here
			phy_env = new DummyPhysicsEnvironment();
			physics_engine = UseNone;
			break;
		}
	}

	destinationscene->SetPhysicsEnvironment(phy_env);

	KX_BlenderSceneConverter sceneConverter;

	ViewLayer *view_layer = BKE_view_layer_default_view(blenderscene);
	Depsgraph *graph = BKE_scene_get_depsgraph(blenderscene, view_layer, false);

	BL_ConvertBlenderObjects(
		m_maggie,
		graph,
		destinationscene,
		m_ketsjiEngine,
		physics_engine,
		rasty,
		canvas,
		sceneConverter,
		m_alwaysUseExpandFraming,
		libloading);

	m_sceneSlots.emplace(destinationscene, sceneConverter);
}

/** This function removes all entities stored in the converter for that scene
 * It should be used instead of direct delete scene
 * Note that there was some provision for sharing entities (meshes...) between
 * scenes but that is now disabled so all scene will have their own copy
 * and we can delete them here. If the sharing is reactivated, change this code too..
 * (see KX_BlenderConverter::ConvertScene)
 */
void KX_BlenderConverter::RemoveScene(KX_Scene *scene)
{
	KX_WorldInfo *world = scene->GetWorldInfo();
	if (world) {
		delete world;
	}

#ifdef WITH_PYTHON
	Texture::FreeAllTextures(scene);
#endif  // WITH_PYTHON

	// delete the scene first as it will stop the use of entities
	scene->Release();

	// delete the entities of this scene
	m_sceneSlots.erase(scene);
}

void KX_BlenderConverter::SetAlwaysUseExpandFraming(bool to_what)
{
	m_alwaysUseExpandFraming = to_what;
}

void KX_BlenderConverter::RegisterInterpolatorList(KX_Scene *scene, BL_InterpolatorList *interpolator, bAction *for_act)
{
	SceneSlot& sceneSlot = m_sceneSlots[scene];
	sceneSlot.m_interpolators.emplace_back(interpolator);
	sceneSlot.m_actionToInterp[for_act] = interpolator;
}

BL_InterpolatorList *KX_BlenderConverter::FindInterpolatorList(KX_Scene *scene, bAction *for_act)
{
	return m_sceneSlots[scene].m_actionToInterp[for_act];
}

Main *KX_BlenderConverter::CreateMainDynamic(const std::string& path)
{
	Main *maggie = BKE_main_new();
	strncpy(maggie->name, path.c_str(), sizeof(maggie->name) - 1);
	m_DynamicMaggie.push_back(maggie);

	return maggie;
}

const std::vector<Main *> &KX_BlenderConverter::GetMainDynamic() const
{
	return m_DynamicMaggie;
}

Main *KX_BlenderConverter::GetMainDynamicPath(const std::string& path) const
{
	for (Main *maggie : m_DynamicMaggie) {
		if (BLI_path_cmp(maggie->name, path.c_str()) == 0) {
			return maggie;
		}
	}

	return nullptr;
}

void KX_BlenderConverter::MergeAsyncLoads()
{
	std::vector<KX_Scene *> *merge_scenes;

	std::vector<KX_LibLoadStatus *>::iterator mit;
	std::vector<KX_Scene *>::iterator sit;

	m_threadinfo.m_mutex.Lock();

	for (mit = m_mergequeue.begin(); mit != m_mergequeue.end(); ++mit) {
		merge_scenes = (std::vector<KX_Scene *> *)(*mit)->GetData();

		for (sit = merge_scenes->begin(); sit != merge_scenes->end(); ++sit) {
			(*mit)->GetMergeScene()->MergeScene(*sit);
			delete (*sit);
		}

		delete merge_scenes;
		(*mit)->SetData(nullptr);

		(*mit)->Finish();
	}

	m_mergequeue.clear();

	m_threadinfo.m_mutex.Unlock();
}

void KX_BlenderConverter::FinalizeAsyncLoads()
{
	// Finish all loading libraries.
	BLI_task_pool_work_and_wait(m_threadinfo.m_pool);
	// Merge all libraries data in the current scene, to avoid memory leak of unmerged scenes.
	MergeAsyncLoads();
}

void KX_BlenderConverter::AddScenesToMergeQueue(KX_LibLoadStatus *status)
{
	m_threadinfo.m_mutex.Lock();
	m_mergequeue.push_back(status);
	m_threadinfo.m_mutex.Unlock();
}

static void async_convert(TaskPool *pool, void *ptr, int UNUSED(threadid))
{
	KX_Scene *new_scene = nullptr;
	KX_LibLoadStatus *status = (KX_LibLoadStatus *)ptr;
	std::vector<Scene *> *scenes = (std::vector<Scene *> *)status->GetData();
	std::vector<KX_Scene *> *merge_scenes = new std::vector<KX_Scene *>(); // Deleted in MergeAsyncLoads

	for (unsigned int i = 0; i < scenes->size(); ++i) {
		new_scene = status->GetEngine()->CreateScene((*scenes)[i], true);

		if (new_scene) {
			merge_scenes->push_back(new_scene);
		}

		status->AddProgress((1.0f / scenes->size()) * 0.9f); // We'll call conversion 90% and merging 10% for now
	}

	delete scenes;
	status->SetData(merge_scenes);

	status->GetConverter()->AddScenesToMergeQueue(status);
}

KX_LibLoadStatus *KX_BlenderConverter::LinkBlendFileMemory(void *data, int length, const char *path, char *group, KX_Scene *scene_merge, char **err_str, short options)
{
	BlendHandle *bpy_openlib = BLO_blendhandle_from_memory(data, length);

	// Error checking is done in LinkBlendFile
	return LinkBlendFile(bpy_openlib, path, group, scene_merge, err_str, options);
}

KX_LibLoadStatus *KX_BlenderConverter::LinkBlendFilePath(const char *filepath, char *group, KX_Scene *scene_merge, char **err_str, short options)
{
	BlendHandle *bpy_openlib = BLO_blendhandle_from_file(filepath, nullptr);

	// Error checking is done in LinkBlendFile
	return LinkBlendFile(bpy_openlib, filepath, group, scene_merge, err_str, options);
}

static void load_datablocks(Main *main_tmp, BlendHandle *bpy_openlib, const char *path, int idcode)
{
	LinkNode *names = nullptr;

	int totnames_dummy;
	names = BLO_blendhandle_get_datablock_names(bpy_openlib, idcode, &totnames_dummy);

	int i = 0;
	LinkNode *n = names;
	while (n) {
		BLO_library_link_named_part(main_tmp, &bpy_openlib, idcode, (char *)n->link);
		n = (LinkNode *)n->next;
		i++;
	}
	BLI_linklist_free(names, free); // free linklist *and* each node's data
}

KX_LibLoadStatus *KX_BlenderConverter::LinkBlendFile(BlendHandle *bpy_openlib, const char *path, char *group, KX_Scene *scene_merge, char **err_str, short options)
{
	Main *main_newlib; // stored as a dynamic 'main' until we free it
	const int idcode = BKE_idcode_from_name(group);
	ReportList reports;
	static char err_local[255];

	KX_LibLoadStatus *status;

	// only scene and mesh supported right now
	if (idcode != ID_SCE && idcode != ID_ME && idcode != ID_AC) {
		snprintf(err_local, sizeof(err_local), "invalid ID type given \"%s\"\n", group);
		*err_str = err_local;
		BLO_blendhandle_close(bpy_openlib);
		return nullptr;
	}

	if (GetMainDynamicPath(path)) {
		snprintf(err_local, sizeof(err_local), "blend file already open \"%s\"\n", path);
		*err_str = err_local;
		BLO_blendhandle_close(bpy_openlib);
		return nullptr;
	}

	if (bpy_openlib == nullptr) {
		snprintf(err_local, sizeof(err_local), "could not open blendfile \"%s\"\n", path);
		*err_str = err_local;
		return nullptr;
	}

	main_newlib = BKE_main_new();
	BKE_reports_init(&reports, RPT_STORE);

	short flag = 0; // don't need any special options
	// created only for linking, then freed
	Main *main_tmp = BLO_library_link_begin(main_newlib, &bpy_openlib, (char *)path);

	load_datablocks(main_tmp, bpy_openlib, path, idcode);

	if (idcode == ID_SCE && options & LIB_LOAD_LOAD_SCRIPTS) {
		load_datablocks(main_tmp, bpy_openlib, path, ID_TXT);
	}

	// now do another round of linking for Scenes so all actions are properly loaded
	if (idcode == ID_SCE && options & LIB_LOAD_LOAD_ACTIONS) {
		load_datablocks(main_tmp, bpy_openlib, path, ID_AC);
	}

	BLO_library_link_end(main_tmp, &bpy_openlib, flag, main_newlib, nullptr, nullptr, nullptr);

	BLO_blendhandle_close(bpy_openlib);

	BKE_reports_clear(&reports);
	// done linking

	// needed for lookups
	m_DynamicMaggie.push_back(main_newlib);
	BLI_strncpy(main_newlib->name, path, sizeof(main_newlib->name));


	status = new KX_LibLoadStatus(this, m_ketsjiEngine, scene_merge, path);

	if (idcode == ID_ME) {
		// Convert all new meshes into BGE meshes
		ID *mesh;

		KX_BlenderSceneConverter sceneConverter;
		for (mesh = (ID *)main_newlib->mesh.first; mesh; mesh = (ID *)mesh->next) {
			if (options & LIB_LOAD_VERBOSE) {
				CM_Debug("mesh name: " << mesh->name + 2);
			}
			RAS_MeshObject *meshobj = BL_ConvertMesh((Mesh *)mesh, nullptr, scene_merge, m_ketsjiEngine->GetRasterizer(), sceneConverter, false); // For now only use the libloading option for scenes, which need to handle materials/shaders
			scene_merge->GetLogicManager()->RegisterMeshName(meshobj->GetName(), meshobj);
		}
		m_sceneSlots[scene_merge].Merge(sceneConverter);
	}
	else if (idcode == ID_AC) {
		// Convert all actions
		ID *action;

		for (action = (ID *)main_newlib->action.first; action; action = (ID *)action->next) {
			if (options & LIB_LOAD_VERBOSE) {
				CM_Debug("action name: " << action->name + 2);
			}
			scene_merge->GetLogicManager()->RegisterActionName(action->name + 2, action);
		}
	}
	else if (idcode == ID_SCE) {
		// Merge all new linked in scene into the existing one
		ID *scene;
		// scenes gets deleted by the thread when it's done using it (look in async_convert())
		std::vector<Scene *> *scenes = (options & LIB_LOAD_ASYNC) ? new std::vector<Scene *>() : nullptr;

		for (scene = (ID *)main_newlib->scene.first; scene; scene = (ID *)scene->next) {
			if (options & LIB_LOAD_VERBOSE) {
				CM_Debug("scene name: " << scene->name + 2);
			}

			if (options & LIB_LOAD_ASYNC) {
				scenes->push_back((Scene *)scene);
			}
			else {
				// merge into the base  scene
				KX_Scene *other = m_ketsjiEngine->CreateScene((Scene *)scene, true);
				scene_merge->MergeScene(other);

				// RemoveScene(other); // Don't run this, it frees the entire scene converter data, just delete the scene
				delete other;
			}
		}

		if (options & LIB_LOAD_ASYNC) {
			status->SetData(scenes);
			BLI_task_pool_push(m_threadinfo.m_pool, async_convert, (void *)status, false, TASK_PRIORITY_LOW);
		}

#ifdef WITH_PYTHON
		// Handle any text datablocks
		if (options & LIB_LOAD_LOAD_SCRIPTS) {
			addImportMain(main_newlib);
		}
#endif

		// Now handle all the actions
		if (options & LIB_LOAD_LOAD_ACTIONS) {
			ID *action;

			for (action = (ID *)main_newlib->action.first; action; action = (ID *)action->next) {
				if (options & LIB_LOAD_VERBOSE) {
					CM_Debug("action name: " << action->name + 2);
				}
				scene_merge->GetLogicManager()->RegisterActionName(action->name + 2, action);
			}
		}
	}

	if (!(options & LIB_LOAD_ASYNC)) {
		status->Finish();
	}

	m_status_map[main_newlib->name] = status;
	return status;
}

/** Note m_map_*** are all ok and don't need to be freed
 * most are temp and NewRemoveObject frees m_map_gameobject_to_blender */
bool KX_BlenderConverter::FreeBlendFile(Main *maggie)
{
	if (maggie == nullptr) {
		return false;
	}

	// If the given library is currently in loading, we do nothing.
	if (m_status_map.count(maggie->name)) {
		m_threadinfo.m_mutex.Lock();
		const bool finished = m_status_map[maggie->name]->IsFinished();
		m_threadinfo.m_mutex.Unlock();

		if (!finished) {
			CM_Error("Library (" << maggie->name << ") is currently being loaded asynchronously, and cannot be freed until this process is done");
			return false;
		}
	}

	// tag all false except the one we remove
	for (std::vector<Main *>::iterator it = m_DynamicMaggie.begin(); it != m_DynamicMaggie.end();) {
		Main *main = *it;
		if (main == maggie) {
			BKE_main_id_tag_all(maggie, LIB_TAG_DOIT, true);
			it = m_DynamicMaggie.erase(it);
		}
		if (main != maggie) {
			BKE_main_id_tag_all(main, LIB_TAG_DOIT, false);
			++it;
		}
	}

	// free all tagged objects
	CListValue<KX_Scene> *scenes = m_ketsjiEngine->CurrentScenes();
	int numScenes = scenes->GetCount();

	for (unsigned int sce_idx = 0; sce_idx < numScenes; ++sce_idx) {
		KX_Scene *scene = scenes->GetValue(sce_idx);
		if (IS_TAGGED(scene->GetBlenderScene())) {
			m_ketsjiEngine->RemoveScene(scene->GetName());
			m_sceneSlots.erase(scene);
			sce_idx--;
			numScenes--;
		}
		else {
			// in case the mesh might be refered to later
			std::map<std::string, void *> &mapStringToMeshes = scene->GetLogicManager()->GetMeshMap();
			for (std::map<std::string, void *>::iterator it = mapStringToMeshes.begin(), end = mapStringToMeshes.end(); it != end;) {
				RAS_MeshObject *meshobj = (RAS_MeshObject *)it->second;
				if (meshobj && IS_TAGGED(meshobj->GetMesh())) {
					it = mapStringToMeshes.erase(it);
				}
				else {
					++it;
				}
			}

			// Now unregister actions.
			std::map<std::string, void *> &mapStringToActions = scene->GetLogicManager()->GetActionMap();
			for (std::map<std::string, void *>::iterator it = mapStringToActions.begin(), end = mapStringToActions.end(); it != end;) {
				ID *action = (ID *)it->second;
				if (IS_TAGGED(action)) {
					it = mapStringToActions.erase(it);
				}
				else {
					++it;
				}
			}

			// removed tagged objects and meshes
			CListValue<KX_GameObject> *obj_lists[] = {scene->GetObjectList(), scene->GetInactiveList(), nullptr};

			for (int ob_ls_idx = 0; obj_lists[ob_ls_idx]; ob_ls_idx++) {
				CListValue<KX_GameObject> *obs = obj_lists[ob_ls_idx];

				for (int ob_idx = 0; ob_idx < obs->GetCount(); ob_idx++) {
					KX_GameObject *gameobj = obs->GetValue(ob_idx);
					if (IS_TAGGED(gameobj->GetBlenderObject())) {
						int size_before = obs->GetCount();

						/* Eventually calls RemoveNodeDestructObject
						 * frees m_map_gameobject_to_blender from UnregisterGameObject */
						scene->RemoveObject(gameobj);

						if (size_before != obs->GetCount()) {
							ob_idx--;
						}
						else {
							CM_Error("could not remove \"" << gameobj->GetName() << "\"");
						}
					}
					else {
						gameobj->RemoveTaggedActions();
						// free the mesh, we could be referecing a linked one!
						int mesh_index = gameobj->GetMeshCount();
						while (mesh_index--) {
							RAS_MeshObject *mesh = gameobj->GetMesh(mesh_index);
							if (IS_TAGGED(mesh->GetMesh())) {
								gameobj->RemoveMeshes(); /* XXX - slack, should only remove meshes that are library items but mostly objects only have 1 mesh */
								break;
							}
							else {
								// also free the mesh if it's using a tagged material
								int mat_index = mesh->NumMaterials();
								while (mat_index--) {
									if (IS_TAGGED(mesh->GetMeshMaterial(mat_index)->GetBucket()->GetPolyMaterial()->GetBlenderMaterial())) {
										gameobj->RemoveMeshes(); // XXX - slack, same as above
										break;
									}
								}
							}
						}

						// make sure action actuators are not referencing tagged actions
						for (unsigned int act_idx = 0; act_idx < gameobj->GetActuators().size(); act_idx++) {
							if (gameobj->GetActuators()[act_idx]->IsType(SCA_IActuator::KX_ACT_ACTION)) {
								BL_ActionActuator *act = (BL_ActionActuator *)gameobj->GetActuators()[act_idx];
								if (IS_TAGGED(act->GetAction())) {
									act->SetAction(nullptr);
								}
							}
						}
					}
				}
			}
		}
	}

	for (std::map<KX_Scene *, SceneSlot>::iterator sit = m_sceneSlots.begin(), send = m_sceneSlots.end(); sit != send; ++sit) {
		KX_Scene *scene = sit->first;
		SceneSlot& sceneSlot = sit->second;

		for (UniquePtrList<KX_BlenderMaterial>::iterator it = sceneSlot.m_materials.begin(); it != sceneSlot.m_materials.end(); ) {
			KX_BlenderMaterial *mat = (*it).get();
			Material *bmat = mat->GetBlenderMaterial();
			if (IS_TAGGED(bmat)) {
				scene->GetBucketManager()->RemoveMaterial(mat);
				it = sceneSlot.m_materials.erase(it);
			}
			else {
				++it;
			}
		}

		for (UniquePtrList<BL_InterpolatorList>::iterator it = sceneSlot.m_interpolators.begin(); it != sceneSlot.m_interpolators.end(); ) {
			BL_InterpolatorList *interp = (*it).get();
			bAction *action = interp->GetAction();
			if (IS_TAGGED(action)) {
				sceneSlot.m_actionToInterp.erase(action);
				it = sceneSlot.m_interpolators.erase(it);
			}
			else {
				++it;
			}
		}

		for (UniquePtrList<RAS_MeshObject>::iterator it =  sceneSlot.m_meshobjects.begin(); it !=  sceneSlot.m_meshobjects.end(); ) {
			RAS_MeshObject *mesh = (*it).get();
			if (IS_TAGGED(mesh->GetMesh())) {
				it = sceneSlot.m_meshobjects.erase(it);
			}
			else {
				++it;
			}
		}
	}

#ifdef WITH_PYTHON
	/* make sure this maggie is removed from the import list if it's there
	 * (this operation is safe if it isn't in the list) */
	removeImportMain(maggie);
#endif

	delete m_status_map[maggie->name];
	m_status_map.erase(maggie->name);

	BKE_main_free(maggie);

	return true;
}

bool KX_BlenderConverter::FreeBlendFile(const std::string& path)
{
	return FreeBlendFile(GetMainDynamicPath(path));
}

void KX_BlenderConverter::MergeScene(KX_Scene *to, KX_Scene *from)
{
	SceneSlot& sceneSlotFrom = m_sceneSlots[from];

	for (std::unique_ptr<KX_BlenderMaterial>& mat : sceneSlotFrom.m_materials) {
		mat->ReplaceScene(to);
	}

	m_sceneSlots[to].Merge(sceneSlotFrom);
	m_sceneSlots.erase(from);

	// Delete from scene's world info.
	delete from->GetWorldInfo();
	from->SetWorldInfo(nullptr);
}

/** This function merges a mesh from the current scene into another main
 * it does not convert */
RAS_MeshObject *KX_BlenderConverter::ConvertMeshSpecial(KX_Scene *kx_scene, Main *maggie, const std::string& name)
{
	// Find a mesh in the current main */
	ID *me = static_cast<ID *>(BLI_findstring(&m_maggie->mesh, name.c_str(), offsetof(ID, name) + 2));
	Main *from_maggie = m_maggie;

	if (me == nullptr) {
		// The mesh wasn't in the current main, try any dynamic (i.e., LibLoaded) ones
		for (Main *main : m_DynamicMaggie) {
			me = static_cast<ID *>(BLI_findstring(&main->mesh, name.c_str(), offsetof(ID, name) + 2));
			from_maggie = main;

			if (me) {
				break;
			}
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
				Material *mat_old = mesh->mat[i];
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

	KX_BlenderSceneConverter sceneConverter;

	RAS_MeshObject *meshobj = BL_ConvertMesh((Mesh *)me, nullptr, kx_scene, m_ketsjiEngine->GetRasterizer(), sceneConverter, false);
	kx_scene->GetLogicManager()->RegisterMeshName(meshobj->GetName(), meshobj);

	m_sceneSlots[kx_scene].Merge(sceneConverter);

	return meshobj;
}

void KX_BlenderConverter::PrintStats()
{
	CM_Message("BGE STATS");
	CM_Message(std::endl << "Assets:");

	unsigned int nummat = 0;
	unsigned int nummesh = 0;
	unsigned int numinter = 0;

	for (const auto& pair : m_sceneSlots) {
		KX_Scene *scene = pair.first;
		const SceneSlot& sceneSlot = pair.second;

		nummat += sceneSlot.m_materials.size();
		nummesh += sceneSlot.m_meshobjects.size();
		numinter += sceneSlot.m_interpolators.size();

		CM_Message("\tscene: " << scene->GetName())
		CM_Message("\t\t materials: " << sceneSlot.m_materials.size());
		CM_Message("\t\t meshes: " << sceneSlot.m_meshobjects.size());
		CM_Message("\t\t interpolators: " << sceneSlot.m_interpolators.size());
	}

	CM_Message(std::endl << "Total:");
	CM_Message("\t scenes: " << m_sceneSlots.size());
	CM_Message("\t materials: " << nummat);
	CM_Message("\t meshes: " << nummesh);
	CM_Message("\t interpolators: " << numinter);
}
