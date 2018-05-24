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
	UniquePtrList<KX_Mesh> m_meshes;
	UniquePtrList<BL_ActionData> m_actions;
	UniquePtrList<BL_ConvertObjectInfo> m_objectInfos;

	NameToResource<KX_Mesh> m_nameToMeshes;
	NameToResource<KX_GameObject> m_nameToObjects;
	NameToResource<BL_ActionData> m_nameToActions;

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
};

#endif  // __BL_RESSOURCE_COLLECTION__
