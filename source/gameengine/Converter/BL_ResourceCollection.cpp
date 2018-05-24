#include "BL_ResourceCollection.h"

#include "KX_Scene.h"
#include "KX_Mesh.h"
#include "KX_GameObject.h"
#include "KX_BlenderMaterial.h"
#include "BL_ScalarInterpolator.h"
#include "BL_SceneConverter.h"
#include "BL_ConvertObjectInfo.h"
#include "RAS_BucketManager.h"

#include "DNA_material_types.h"
#include "DNA_action_types.h"

#include "BKE_library.h"

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

	for (bAction *action : converter.m_actions) {
		m_nameToActions[action->id.name + 2] = action;
	}

	for (const auto& pair : converter.m_map_blender_to_gameobject) {
		KX_GameObject *obj = pair.second;
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
	m_interpolators.insert(m_interpolators.begin(),
	                     std::make_move_iterator(other.m_interpolators.begin()),
	                     std::make_move_iterator(other.m_interpolators.end()));
	m_objectInfos.insert(m_objectInfos.begin(),
	                     std::make_move_iterator(other.m_objectInfos.begin()),
	                     std::make_move_iterator(other.m_objectInfos.end()));

	m_nameToMeshes.insert(other.m_nameToMeshes.begin(), other.m_nameToMeshes.end());
	m_nameToObjects.insert(other.m_nameToObjects.begin(), other.m_nameToObjects.end());
	m_nameToActions.insert(other.m_nameToActions.begin(), other.m_nameToActions.end());
	m_actionToInterp.insert(other.m_actionToInterp.begin(), other.m_actionToInterp.end());
}

void BL_ResourceCollection::RemoveTagged(KX_Scene *scene)
{
	for (auto it = m_meshes.begin(); it !=  m_meshes.end(); ) {
		KX_Mesh *mesh = it->get();
		if (IS_TAGGED(mesh->GetMesh())) {
			it = m_meshes.erase(it);
		}
		else {
			++it;
		}
	}

	for (auto it = m_materials.begin(); it != m_materials.end(); ) {
		KX_BlenderMaterial *mat = it->get();
		Material *bmat = mat->GetBlenderMaterial();
		if (IS_TAGGED(bmat)) {
			scene->GetBucketManager()->RemoveMaterial(mat);
			it = m_materials.erase(it);
		}
		else {
			++it;
		}
	}

	for (auto it = m_interpolators.begin(); it != m_interpolators.end(); ) {
		BL_InterpolatorList *interp = it->get();
		bAction *action = interp->GetAction();
		if (IS_TAGGED(action)) {
			m_actionToInterp.erase(action);
			it = m_interpolators.erase(it);
		}
		else {
			++it;
		}
	}

	for (auto it = m_nameToMeshes.begin(), end = m_nameToMeshes.end(); it != end; ) {
		KX_Mesh *meshobj = it->second;
		if (meshobj && IS_TAGGED(meshobj->GetMesh())) {
			it = m_nameToMeshes.erase(it);
		}
		else {
			++it;
		}
	}

	for (auto it = m_nameToActions.begin(), end = m_nameToActions.end(); it != end; ) {
		bAction *action = it->second;
		if (IS_TAGGED(action)) {
			it = m_nameToActions.erase(it);
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
	m_interpolators.clear();
	m_objectInfos.clear();
}

void BL_ResourceCollection::RegisterMesh(KX_Mesh* mesh)
{
	m_meshes.emplace_back(mesh);
}

void BL_ResourceCollection::RegisterInterpolatorList(bAction *action, BL_InterpolatorList *interpolator)
{
	m_interpolators.emplace_back(interpolator);
	m_actionToInterp[action] = interpolator;
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

KX_Mesh *BL_ResourceCollection::FindMesh(const std::string& name)
{
	const auto it = m_nameToMeshes.find(name);
	return (it != m_nameToMeshes.end()) ? it->second : nullptr;
}

KX_GameObject *BL_ResourceCollection::FindObject(const std::string& name)
{
	const auto it = m_nameToObjects.find(name);
	return (it != m_nameToObjects.end()) ? it->second : nullptr;
}

bAction  *BL_ResourceCollection::FindAction(const std::string& name)
{
	const auto it = m_nameToActions.find(name);
	return (it != m_nameToActions.end()) ? it->second : nullptr;
}

BL_InterpolatorList *BL_ResourceCollection::FindInterpolatorList(bAction *action)
{
	const auto it = m_actionToInterp.find(action);
	return (it != m_actionToInterp.end()) ? it->second : nullptr;
}
