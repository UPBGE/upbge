#include "BL_ResourceCollection.h"

#include "KX_Scene.h"
#include "KX_Mesh.h"
#include "KX_GameObject.h"
#include "KX_BlenderMaterial.h"
#include "BL_ActionData.h"
#include "BL_SceneConverter.h"
#include "BL_ConvertObjectInfo.h"
#include "RAS_BucketManager.h"

#include "CM_Map.h"

BL_ResourceCollection::BL_ResourceCollection() = default;

BL_ResourceCollection::BL_ResourceCollection(const BL_SceneConverter& converter)
{
	for (KX_BlenderMaterial *mat : converter.m_materials) {
		m_materials.emplace_back(mat);
	}
	for (KX_Mesh *mesh : converter.m_meshes) {
		m_meshes.emplace_back(mesh);
		m_nameToMeshes[mesh->GetName()] = mesh;
	}
	for (BL_ConvertObjectInfo *info : converter.m_objectInfos) {
		m_objectInfos.emplace_back(info);
	}

	for (BL_ActionData *action : converter.m_actions) {
		m_nameToActions[action->GetName()] = action;
	}

	for (KX_GameObject *obj : converter.m_objects) {
		m_nameToObjects[obj->GetName()] = obj;
	}
}

BL_ResourceCollection::~BL_ResourceCollection() = default;

BL_ResourceCollection::BL_ResourceCollection(BL_ResourceCollection&& other) = default;

BL_ResourceCollection& BL_ResourceCollection::operator=(BL_ResourceCollection&& other) = default;

void BL_ResourceCollection::Merge(BL_ResourceCollection& other)
{
	m_materials.insert(m_materials.begin(),
	                   std::make_move_iterator(other.m_materials.begin()),
	                   std::make_move_iterator(other.m_materials.end()));
	m_meshes.insert(m_meshes.begin(),
	                     std::make_move_iterator(other.m_meshes.begin()),
	                     std::make_move_iterator(other.m_meshes.end()));
	m_actions.insert(m_actions.begin(),
	                     std::make_move_iterator(other.m_actions.begin()),
	                     std::make_move_iterator(other.m_actions.end()));
	m_objectInfos.insert(m_objectInfos.begin(),
	                     std::make_move_iterator(other.m_objectInfos.begin()),
	                     std::make_move_iterator(other.m_objectInfos.end()));

	m_nameToMeshes.insert(other.m_nameToMeshes.begin(), other.m_nameToMeshes.end());
	m_nameToObjects.insert(other.m_nameToObjects.begin(), other.m_nameToObjects.end());
	m_nameToActions.insert(other.m_nameToActions.begin(), other.m_nameToActions.end());
}

void BL_ResourceCollection::RemoveResources(BL_Resource::Library libraryId, KX_Scene *scene)
{
	// Free meshes.
	for (UniquePtrList<KX_Mesh>::iterator it =  m_meshes.begin(); it !=  m_meshes.end(); ) {
		KX_Mesh *mesh = it->get();
		if (mesh->Belong(libraryId)) {
			it = m_meshes.erase(it);
		}
		else {
			++it;
		}
	}

	// Free materials.
	for (UniquePtrList<KX_BlenderMaterial>::iterator it = m_materials.begin(); it != m_materials.end(); ) {
		KX_BlenderMaterial *mat = it->get();
		if (mat->Belong(libraryId)) {
			scene->GetBucketManager()->RemoveMaterial(mat);
			it = m_materials.erase(it);
		}
		else {
			++it;
		}
	}

	// Free actions.
	for (UniquePtrList<BL_ActionData>::iterator it = m_actions.begin(); it != m_actions.end(); ) {
		BL_ActionData *act = it->get();
		if (act->Belong(libraryId)) {
			it = m_actions.erase(it);
		}
		else {
			++it;
		}
	}

	// Free object infos.
	for (UniquePtrList<BL_ConvertObjectInfo>::iterator it = m_objectInfos.begin(); it != m_objectInfos.end(); ) {
		BL_ConvertObjectInfo *info = it->get();
		if (info->Belong(libraryId)) {
			it = m_objectInfos.erase(it);
		}
		else {
			++it;
		}
	}
}

void BL_ResourceCollection::Clear()
{
	m_materials.clear();
	m_meshes.clear();
	m_actions.clear();
	m_objectInfos.clear();
}

void BL_ResourceCollection::RegisterMesh(KX_Mesh* mesh)
{
	m_meshes.emplace_back(mesh);
}

void BL_ResourceCollection::UnregisterObject(const std::string& name)
{
	m_nameToObjects.erase(name);
}

bool BL_ResourceCollection::ChangeObjectName(const std::string& oldname, const std::string& newname, KX_GameObject *object)
{
	auto it = m_nameToObjects.find(oldname);
	if (it == m_nameToObjects.end() || it->second != object) {
		return true;
	}

	/* Two non-replica objects can't have the same name because these objects are register in the
	 * logic manager and that the result of FindObject will be undefined. */
	if (m_nameToObjects.find(newname) != m_nameToObjects.end()) {
		return false;
	}

	// Unregister the old name.
	m_nameToObjects.erase(it);
	// Register the object under the new name.
	m_nameToObjects.emplace(newname, object);

	return true;
}

KX_Mesh *BL_ResourceCollection::FindMesh(const std::string& name) const
{
	return CM_MapGetItemNoInsert(m_nameToMeshes, name);
}

KX_GameObject *BL_ResourceCollection::FindObject(const std::string& name) const
{
	return CM_MapGetItemNoInsert(m_nameToObjects, name);
}

BL_ActionData  *BL_ResourceCollection::FindAction(const std::string& name) const
{
	return CM_MapGetItemNoInsert(m_nameToActions, name);
}
