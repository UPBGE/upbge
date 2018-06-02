#ifndef __LOD_USER_H__
#define __LOD_USER_H__

#include <vector>
#include <memory>

class KX_LodManager;
class KX_ClientObjectInfo;
class KX_Scene;
class KX_GameObject;
class RAS_MeshUser;

class KX_LodUser
{
private:
	std::vector<std::unique_ptr<RAS_MeshUser> > m_meshUsers;

	KX_LodManager *m_manager;

	unsigned int m_currentLevel;

public:
	KX_LodUser();
	KX_LodUser(KX_LodManager *manager);
	KX_LodUser(const KX_LodUser& other);
	~KX_LodUser();

	void operator= (const KX_LodUser& other);

	bool Valid() const;
	KX_LodManager *GetManager() const;
	RAS_MeshUser *GetMesh(KX_GameObject *object, KX_ClientObjectInfo& info, KX_Scene *scene, float distance2);
};


#endif  // __LOD_USER_H__
