#ifndef __BL_RESSOURCE_COLLECTION__
#define __BL_RESSOURCE_COLLECTION__

#include <map>
#include <string>
#include <vector>
#include <memory>

class KX_Scene;
class KX_Mesh;
class KX_GameObject;
class KX_BlenderMaterial;
class BL_InterpolatorList;
class BL_ConvertObjectInfo;
class BL_SceneConverter;
struct bAction;

/** \brief This class own the resources of a scene and name map to objects.
 * Resources are mesh, materials and actions.
 * Name map for object, meshes and actions are used for python API.
 */
class BL_ResourceCollection
{
public:
	template<class Value>
	using UniquePtrList = std::vector<std::unique_ptr<Value> >;

	template <class Resource>
	using NameToResource = std::map<std::string, Resource *>;

private:
	UniquePtrList<KX_BlenderMaterial> m_materials;
	UniquePtrList<KX_Mesh> m_meshes;
	UniquePtrList<BL_InterpolatorList> m_interpolators;
	UniquePtrList<BL_ConvertObjectInfo> m_objectInfos;

	NameToResource<KX_Mesh> m_nameToMeshes;
	NameToResource<KX_GameObject> m_nameToObjects;
	NameToResource<bAction> m_nameToActions;
	std::map<bAction *, BL_InterpolatorList *> m_actionToInterp;

public:
	BL_ResourceCollection();
	~BL_ResourceCollection();
	BL_ResourceCollection(const BL_SceneConverter& converter);
	BL_ResourceCollection(const BL_ResourceCollection& other) = delete;
	BL_ResourceCollection(BL_ResourceCollection&& other);

	BL_ResourceCollection& operator=(const BL_ResourceCollection& other) = delete;
	BL_ResourceCollection& operator=(BL_ResourceCollection&& other);

	void Merge(BL_ResourceCollection& other);
	/// Remove all data tagged during a file free.
	void RemoveTagged(KX_Scene *scene);
	/// Free all resources.
	void Clear();

	/// Register a mesh object copy.
	void RegisterMesh(KX_Mesh *mesh);
	void RegisterInterpolatorList(bAction *action, BL_InterpolatorList *interpolator);

	void UnregisterObject(const std::string& name);
	bool ChangeObjectName(const std::string& oldname, const std::string& newname, KX_GameObject *object);

	KX_Mesh *FindMesh(const std::string& name);
	KX_GameObject *FindObject(const std::string& name);
	bAction *FindAction(const std::string& name);
	BL_InterpolatorList *FindInterpolatorList(bAction *action);
};

#endif  // __BL_RESSOURCE_COLLECTION__
