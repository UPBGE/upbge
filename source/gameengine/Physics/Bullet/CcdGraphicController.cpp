/** \file gameengine/Physics/Bullet/CcdGraphicController.cpp
 *  \ingroup physbullet
 */
/*
   Bullet Continuous Collision Detection and Physics Library
   Copyright (c) 2003-2006 Erwin Coumans  http://continuousphysics.com/Bullet/

   This software is provided 'as-is', without any express or implied warranty.
   In no event will the authors be held liable for any damages arising from the use of this software.
   Permission is granted to anyone to use this software for any purpose,
   including commercial applications, and to alter it and redistribute it freely,
   subject to the following restrictions:

   1. The origin of this software must not be misrepresented; you must not claim that you wrote the original software. If you use this software in a product, an acknowledgment in the product documentation would be appreciated but is not required.
   2. Altered source versions must be plainly marked as such, and must not be misrepresented as being the original software.
   3. This notice may not be removed or altered from any source distribution.
 */

#include "CcdPhysicsEnvironment.h"
#include "CcdGraphicController.h"
#include "btBulletDynamicsCommon.h"

CcdGraphicController::CcdGraphicController(CcdPhysicsEnvironment *phyEnv, PHY_IMotionState *motionState)
	:m_localAabbMin(0.0f, 0.0f, 0.0f),
	m_localAabbMax(0.0f, 0.0f, 0.0f),
	m_motionState(motionState),
	m_phyEnv(phyEnv),
	m_handle(nullptr),
	m_newClientInfo(nullptr)
{
}

CcdGraphicController::~CcdGraphicController()
{
	if (m_phyEnv)
		m_phyEnv->RemoveCcdGraphicController(this);

	if (m_motionState)
		delete m_motionState;
}

void CcdGraphicController::SetLocalAabb(const btVector3& aabbMin, const btVector3& aabbMax)
{
	m_localAabbMin = aabbMin;
	m_localAabbMax = aabbMax;
	SetGraphicTransform();
}

void CcdGraphicController::SetLocalAabb(const mt::vec3& aabbMin, const mt::vec3& aabbMax)
{
	m_localAabbMin = ToBullet(aabbMin);
	m_localAabbMax = ToBullet(aabbMax);
	SetGraphicTransform();
}

void CcdGraphicController::GetAabb(btVector3& aabbMin, btVector3& aabbMax)
{
	const btVector3 pos = ToBullet(m_motionState->GetWorldPosition());
	const btVector3 scale = ToBullet(m_motionState->GetWorldScaling());
	const btMatrix3x3 rot = ToBullet(m_motionState->GetWorldOrientation());

	btVector3 localAabbMin = m_localAabbMin;
	btVector3 localAabbMax = m_localAabbMax;
	btVector3 tmpAabbMin = m_localAabbMin * scale;
	btVector3 tmpAabbMax = m_localAabbMax * scale;

	localAabbMin[0] = (scale.getX() >= 0.0f) ? tmpAabbMin[0] : tmpAabbMax[0];
	localAabbMin[1] = (scale.getY() >= 0.0f) ? tmpAabbMin[1] : tmpAabbMax[1];
	localAabbMin[2] = (scale.getZ() >= 0.0f) ? tmpAabbMin[2] : tmpAabbMax[2];
	localAabbMax[0] = (scale.getX() <= 0.0f) ? tmpAabbMin[0] : tmpAabbMax[0];
	localAabbMax[1] = (scale.getY() <= 0.0f) ? tmpAabbMin[1] : tmpAabbMax[1];
	localAabbMax[2] = (scale.getZ() <= 0.0f) ? tmpAabbMin[2] : tmpAabbMax[2];

	btVector3 localHalfExtents = btScalar(0.5f) * (localAabbMax - localAabbMin);
	btVector3 localCenter = btScalar(0.5f) * (localAabbMax + localAabbMin);

	btMatrix3x3 abs_b = rot.absolute();
	btVector3 center = rot * localCenter + pos;
	btVector3 extent = abs_b * localHalfExtents;
	aabbMin = center - extent;
	aabbMax = center + extent;
}

bool CcdGraphicController::SetGraphicTransform()
{
	if (!m_handle)
		return false;
	btVector3 aabbMin;
	btVector3 aabbMax;
	GetAabb(aabbMin, aabbMax);
	// update Aabb in broadphase
	m_phyEnv->GetCullingTree()->setAabb(m_handle, aabbMin, aabbMax, nullptr);
	return true;
}

PHY_IGraphicController *CcdGraphicController::GetReplica(class PHY_IMotionState *motionState)
{
	CcdGraphicController *replica = new CcdGraphicController(*this);
	replica->m_motionState = motionState;
	replica->m_newClientInfo = nullptr;
	replica->m_handle = nullptr;
	// don't add the graphic controller now: work around a bug in Bullet with rescaling,
	// (the scale of the controller is not yet defined).
	//m_phyEnv->addCcdGraphicController(replica);
	return replica;
}

void CcdGraphicController::SetPhysicsEnvironment(class PHY_IPhysicsEnvironment *env)
{
	CcdPhysicsEnvironment *phyEnv = static_cast<CcdPhysicsEnvironment *>(env);
	// Updates the m_phyEnv's m_cullingTree & m_cullingCache
	if (GetBroadphaseHandle()) {
		// insert into the new physics scene
		Activate(false);
		m_phyEnv = phyEnv;
		Activate(true);
	}
	else {
		m_phyEnv = phyEnv;
	}
}

void CcdGraphicController::Activate(bool active)
{
	if (active)
		m_phyEnv->AddCcdGraphicController(this);
	else
		m_phyEnv->RemoveCcdGraphicController(this);
}
