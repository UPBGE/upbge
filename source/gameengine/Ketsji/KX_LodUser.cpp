#include "BL_BlenderDataConversion.h" // For BL_ConvertDeformer.

#include "KX_LodUser.h"
#include "KX_LodLevel.h"
#include "KX_LodManager.h"
#include "KX_GameObject.h"
#include "KX_Mesh.h"

#include "RAS_MeshUser.h"

KX_LodUser::KX_LodUser()
	:KX_LodUser(nullptr)
{
}

KX_LodUser::KX_LodUser(KX_LodManager *manager)
	:m_manager(manager),
	m_currentLevel(0)
{
	if (m_manager) {
		m_manager->AddRef();
		m_meshUsers.resize(m_manager->GetLevelCount());
	}
}

KX_LodUser::KX_LodUser(const KX_LodUser& other)
	:KX_LodUser(other.m_manager)
{
}

KX_LodUser::~KX_LodUser()
{
	if (m_manager) {
		m_manager->Release();
	}
}

void KX_LodUser::operator=(const KX_LodUser& other)
{
	m_currentLevel = 0;
	m_meshUsers.clear();

	if (m_manager) {
		m_manager->Release();
	}

	m_manager = other.m_manager;

	if (m_manager) {
		m_manager->AddRef();
		m_meshUsers.resize(m_manager->GetLevelCount());
	}
}

bool KX_LodUser::Valid() const
{
	return (m_manager != nullptr);
}

KX_LodManager *KX_LodUser::GetManager() const
{
	return m_manager;
}

RAS_MeshUser *KX_LodUser::GetMesh(KX_GameObject *object, KX_ClientObjectInfo& info, KX_Scene *scene, float distance2)
{
	const KX_LodLevel& level = m_manager->GetLevel(scene, m_currentLevel, distance2);

	const unsigned short index = level.GetLevel();
	if (index == m_currentLevel) {
		return nullptr;
	}

	if (!m_meshUsers[index]) {
		KX_Mesh *mesh = level.GetMesh();
		RAS_Deformer *deformer = BL_ConvertDeformer(object, mesh);
		m_meshUsers[index].reset(mesh->AddMeshUser(&info, deformer));
	}

	m_currentLevel = index;

	return m_meshUsers[index].get();
}
