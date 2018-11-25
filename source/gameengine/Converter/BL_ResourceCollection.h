#ifndef __BL_RESSOURCE_COLLECTION__
#define __BL_RESSOURCE_COLLECTION__

#include <map>
#include <string>
#include <vector>
#include <memory>

#include "BL_Resource.h"

class KX_Scene;
class KX_Mesh;
class KX_GameObject;
class KX_BlenderMaterial;
class BL_ActionData;
class BL_ConvertObjectInfo;
class BL_SceneConverter;
class BL_Converter;

/** \brief This class own the resources of a scene and name map to objects.
 * Resources are mesh, materials and actions.
 * Name map for object, meshes and actions are used for python API.
 */
class BL_ResourceCollection
{
friend BL_Converter;

public:
	template<class Value>
	using UniquePtrList = std::vector<std::unique_ptr<Value> >;

	template <class Resource>
	using NameToResource = std::map<std::string, Resource *>;

private:
	UniquePtrList<KX_BlenderMaterial> m_materials;
	NameToResource<KX_BlenderMaterial> m_nameToMaterials;

	UniquePtrList<KX_Mesh> m_meshes;
	NameToResource<KX_Mesh> m_nameToMeshes;

	UniquePtrList<BL_ActionData> m_actions;
	NameToResource<BL_ActionData> m_nameToActions;

	UniquePtrList<BL_ConvertObjectInfo> m_objectInfos;
	NameToResource<KX_GameObject> m_nameToObjects;

public:
	BL_ResourceCollection();
	~BL_ResourceCollection();
	BL_ResourceCollection(const BL_SceneConverter& converter);
	BL_ResourceCollection(const BL_ResourceCollection& other) = delete;
	BL_ResourceCollection(BL_ResourceCollection&& other);

	BL_ResourceCollection& operator=(const BL_ResourceCollection& other) = delete;
	BL_ResourceCollection& operator=(BL_ResourceCollection&& other);

	void Merge(BL_ResourceCollection& other);
	/// Remove resources owned by the following library for a scene.
	void RemoveResources(BL_Resource::Library libraryId, KX_Scene *scene);
	/// Free all resources.
	void Clear();

	/// Register a mesh object copy.
	void RegisterMesh(KX_Mesh *mesh);

	void UnregisterObject(const std::string& name);
	bool ChangeObjectName(const std::string& oldname, const std::string& newname, KX_GameObject *object);

	KX_Mesh *FindMesh(const std::string& name) const;
	KX_GameObject *FindObject(const std::string& name) const;
	BL_ActionData *FindAction(const std::string& name) const;
	KX_BlenderMaterial *FindMaterial(const std::string& name) const;

	// Template version of Find[Object] functions.
	template <class Object>
	Object *Find(const std::string& name) const;
};

template <>
inline KX_Mesh *BL_ResourceCollection::Find<KX_Mesh>(const std::string& name) const
{
	return FindMesh(name);
}

template <>
inline KX_GameObject *BL_ResourceCollection::Find<KX_GameObject>(const std::string& name) const
{
	return FindObject(name);
}

template <>
inline BL_ActionData *BL_ResourceCollection::Find<BL_ActionData>(const std::string& name) const
{
	return FindAction(name);
}

template <>
inline KX_BlenderMaterial *BL_ResourceCollection::Find<KX_BlenderMaterial>(const std::string& name) const
{
	return FindMaterial(name);
}

#endif  // __BL_RESSOURCE_COLLECTION__
