/** \file gameengine/Physics/Bullet/CcdPhysicsEnvironment.cpp
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
#include "CcdPhysicsController.h"
#include "CcdGraphicController.h"
#include "CcdConstraint.h"
#include "CcdMathUtils.h"

#include <algorithm>
#include "btBulletDynamicsCommon.h"
#include "LinearMath/btIDebugDraw.h"
#include "BulletCollision/CollisionDispatch/btGhostObject.h"
#include "BulletCollision/CollisionDispatch/btSimulationIslandManager.h"
#include "BulletCollision/CollisionDispatch/btCollisionDispatcherMt.h"
#include "BulletSoftBody/btSoftRigidDynamicsWorldMt.h"
#include "BulletSoftBody/btSoftBodyRigidBodyCollisionConfiguration.h"
#include "BulletCollision/Gimpact/btGImpactCollisionAlgorithm.h"
#include "BulletCollision/NarrowPhaseCollision/btRaycastCallback.h"
#include "BulletDynamics/ConstraintSolver/btSequentialImpulseConstraintSolverMt.h"
#include "BulletDynamics/ConstraintSolver/btNNCGConstraintSolver.h"
#include "BulletDynamics/MLCPSolvers/btMLCPSolver.h"
#include "BulletDynamics/MLCPSolvers/btDantzigSolver.h"
#include "BulletDynamics/MLCPSolvers/btLemkeSolver.h"
#include "BulletDynamics/MLCPSolvers/btPATHSolver.h"

//profiling/timings
#include "LinearMath/btQuickprof.h"


#include "PHY_IMotionState.h"
#include "PHY_ICharacter.h"
#include "KX_GameObject.h"
#include "KX_Globals.h" // for KX_RasterizerDrawDebugLine
#include "KX_Mesh.h"
#include "BL_SceneConverter.h"
#include "RAS_DisplayArray.h"
#include "RAS_MaterialBucket.h"
#include "RAS_IPolygonMaterial.h"

#include "DNA_scene_types.h"
#include "DNA_world_types.h"
#include "DNA_object_types.h" // for OB_MAX_COL_MASKS
#include "DNA_object_force_types.h"

extern "C" {
	#include "BLI_utildefines.h"
	#include "BKE_object.h"
}

#define CCD_CONSTRAINT_DISABLE_LINKED_COLLISION 0x80

#include "BulletDynamics/Vehicle/btRaycastVehicle.h"
#include "BulletDynamics/Vehicle/btVehicleRaycaster.h"
#include "BulletDynamics/Vehicle/btWheelInfo.h"
#include "PHY_IVehicle.h"
static btRaycastVehicle::btVehicleTuning gTuning;

#include "LinearMath/btAabbUtil2.h"

#ifdef WIN32
void DrawRasterizerLine(const float *from, const float *to, int color);
#endif


#include "BulletDynamics/ConstraintSolver/btContactConstraint.h"

#include "CM_Message.h"
#include "CM_List.h"

// This was copied from the old KX_ConvertPhysicsObjects
#ifdef WIN32
#ifdef _MSC_VER
//only use SIMD Hull code under Win32
//#define TEST_HULL 1
#ifdef TEST_HULL
#define USE_HULL 1
//#define TEST_SIMD_HULL 1

#include "NarrowPhaseCollision/Hull.h"
#endif //#ifdef TEST_HULL

#endif //_MSC_VER
#endif //WIN32

class VehicleClosestRayResultCallback : public btCollisionWorld::ClosestRayResultCallback
{
private:
	const btCollisionShape *m_hitTriangleShape;
	unsigned short m_mask;

public:
	VehicleClosestRayResultCallback(const btVector3& rayFrom, const btVector3& rayTo, unsigned short mask)
		:btCollisionWorld::ClosestRayResultCallback(rayFrom, rayTo),
		m_mask(mask)
	{
	}

	virtual ~VehicleClosestRayResultCallback()
	{
	}

	virtual bool needsCollision(btBroadphaseProxy *proxy0) const
	{
		if (!btCollisionWorld::ClosestRayResultCallback::needsCollision(proxy0)) {
			return false;
		}

		btCollisionObject *object = (btCollisionObject *)proxy0->m_clientObject;
		CcdPhysicsController *phyCtrl = static_cast<CcdPhysicsController *>(object->getUserPointer());

		if (phyCtrl->GetCollisionGroup() & m_mask) {
			return true;
		}

		return false;
	}
};

class BlenderVehicleRaycaster : public btDefaultVehicleRaycaster
{
private:
	btDynamicsWorld *m_dynamicsWorld;
	unsigned short m_mask;

public:
	BlenderVehicleRaycaster(btDynamicsWorld *world)
		:btDefaultVehicleRaycaster(world),
		m_dynamicsWorld(world),
		m_mask((1 << OB_MAX_COL_MASKS) - 1)
	{
	}

	virtual void *castRay(const btVector3& from, const btVector3& to, btVehicleRaycasterResult& result)
	{
		//	RayResultCallback& resultCallback;

		VehicleClosestRayResultCallback rayCallback(from, to, m_mask);

		// We override btDefaultVehicleRaycaster so we can set this flag, otherwise our
		// vehicles go crazy (http://bulletphysics.org/Bullet/phpBB3/viewtopic.php?t=9662)
		rayCallback.m_flags |= btTriangleRaycastCallback::kF_UseSubSimplexConvexCastRaytest;

		m_dynamicsWorld->rayTest(from, to, rayCallback);

		if (rayCallback.hasHit()) {
			const btRigidBody *body = btRigidBody::upcast(rayCallback.m_collisionObject);
			if (body && body->hasContactResponse()) {
				result.m_hitPointInWorld = rayCallback.m_hitPointWorld;
				result.m_hitNormalInWorld = rayCallback.m_hitNormalWorld;
				result.m_hitNormalInWorld.normalize();
				result.m_distFraction = rayCallback.m_closestHitFraction;
				return (void *)body;
			}
		}
		return nullptr;
	}

	unsigned short GetRayCastMask() const
	{
		return m_mask;
	}

	void SetRayCastMask(unsigned short mask)
	{
		m_mask = mask;
	}
};

class WrapperVehicle : public PHY_IVehicle
{
	btRaycastVehicle *m_vehicle;
	BlenderVehicleRaycaster *m_raycaster;
	PHY_IPhysicsController *m_chassis;

public:
	WrapperVehicle(btRaycastVehicle *vehicle, BlenderVehicleRaycaster *raycaster, PHY_IPhysicsController *chassis)
		:m_vehicle(vehicle),
		m_raycaster(raycaster),
		m_chassis(chassis)
	{
	}

	~WrapperVehicle()
	{
		for (unsigned short i = 0, numWheels = GetNumWheels(); i < numWheels; ++i) {
			btWheelInfo& info = m_vehicle->getWheelInfo(i);
			PHY_IMotionState *motionState = (PHY_IMotionState *)info.m_clientInfo;
			delete motionState;
		}

		delete m_vehicle;
		delete m_raycaster;
	}

	btRaycastVehicle *GetVehicle()
	{
		return m_vehicle;
	}

	PHY_IPhysicsController *GetChassis()
	{
		return m_chassis;
	}

	virtual void AddWheel(PHY_IMotionState *motionState,
	                      const mt::vec3 &connectionPoint,
	                      const mt::vec3 &downDirection,
	                      const mt::vec3 &axleDirection,
	                      float suspensionRestLength,
	                      float wheelRadius,
	                      bool hasSteering)
	{
		btWheelInfo& info = m_vehicle->addWheel(ToBullet(connectionPoint), ToBullet(downDirection.Normalized()),
		                                        ToBullet(axleDirection.Normalized()), suspensionRestLength, wheelRadius, gTuning, hasSteering);
		info.m_clientInfo = motionState;
	}

	void SyncWheels()
	{
		int numWheels = GetNumWheels();
		int i;
		for (i = 0; i < numWheels; i++) {
			btWheelInfo& info = m_vehicle->getWheelInfo(i);
			PHY_IMotionState *motionState = (PHY_IMotionState *)info.m_clientInfo;
			m_vehicle->updateWheelTransform(i, false);
			const btTransform trans = m_vehicle->getWheelInfo(i).m_worldTransform;
			motionState->SetWorldOrientation(ToMt(trans.getBasis()));
			motionState->SetWorldPosition(ToMt(trans.getOrigin()));
		}
	}

	virtual int GetNumWheels() const
	{
		return m_vehicle->getNumWheels();
	}

	virtual mt::vec3 GetWheelPosition(int wheelIndex) const
	{
		if ((wheelIndex >= 0) && (wheelIndex < m_vehicle->getNumWheels())) {
			const btVector3 origin = m_vehicle->getWheelTransformWS(wheelIndex).getOrigin();
			return ToMt(origin);
		}
		return mt::zero3;
	}

	virtual mt::quat GetWheelOrientationQuaternion(int wheelIndex) const
	{
		if ((wheelIndex >= 0) && (wheelIndex < m_vehicle->getNumWheels())) {
			const btQuaternion quat = m_vehicle->getWheelTransformWS(wheelIndex).getRotation();
			return ToMt(quat);
		}
		return mt::quat(0.0f, 0.0f, 0.0f, 0.0f);
	}

	virtual float GetWheelRotation(int wheelIndex) const
	{
		float rotation = 0.0f;

		if ((wheelIndex >= 0) && (wheelIndex < m_vehicle->getNumWheels())) {
			btWheelInfo& info = m_vehicle->getWheelInfo(wheelIndex);
			rotation = info.m_rotation;
		}

		return rotation;
	}

	virtual int GetUserConstraintId() const
	{
		return m_vehicle->getUserConstraintId();
	}

	virtual int GetUserConstraintType() const
	{
		return m_vehicle->getUserConstraintType();
	}

	virtual void SetSteeringValue(float steering, int wheelIndex)
	{
		if ((wheelIndex >= 0) && (wheelIndex < m_vehicle->getNumWheels())) {
			m_vehicle->setSteeringValue(steering, wheelIndex);
		}
	}

	virtual void ApplyEngineForce(float force, int wheelIndex)
	{
		if ((wheelIndex >= 0) && (wheelIndex < m_vehicle->getNumWheels())) {
			m_vehicle->applyEngineForce(force, wheelIndex);
		}
	}

	virtual void ApplyBraking(float braking, int wheelIndex)
	{
		if ((wheelIndex >= 0) && (wheelIndex < m_vehicle->getNumWheels())) {
			btWheelInfo& info = m_vehicle->getWheelInfo(wheelIndex);
			info.m_brake = braking;
		}
	}

	virtual void SetWheelFriction(float friction, int wheelIndex)
	{
		if ((wheelIndex >= 0) && (wheelIndex < m_vehicle->getNumWheels())) {
			btWheelInfo& info = m_vehicle->getWheelInfo(wheelIndex);
			info.m_frictionSlip = friction;
		}
	}

	virtual void SetSuspensionStiffness(float suspensionStiffness, int wheelIndex)
	{
		if ((wheelIndex >= 0) && (wheelIndex < m_vehicle->getNumWheels())) {
			btWheelInfo& info = m_vehicle->getWheelInfo(wheelIndex);
			info.m_suspensionStiffness = suspensionStiffness;
		}
	}

	virtual void SetSuspensionDamping(float suspensionDamping, int wheelIndex)
	{
		if ((wheelIndex >= 0) && (wheelIndex < m_vehicle->getNumWheels())) {
			btWheelInfo& info = m_vehicle->getWheelInfo(wheelIndex);
			info.m_wheelsDampingRelaxation = suspensionDamping;
		}
	}

	virtual void SetSuspensionCompression(float suspensionCompression, int wheelIndex)
	{
		if ((wheelIndex >= 0) && (wheelIndex < m_vehicle->getNumWheels())) {
			btWheelInfo& info = m_vehicle->getWheelInfo(wheelIndex);
			info.m_wheelsDampingCompression = suspensionCompression;
		}
	}

	virtual void SetRollInfluence(float rollInfluence, int wheelIndex)
	{
		if ((wheelIndex >= 0) && (wheelIndex < m_vehicle->getNumWheels())) {
			btWheelInfo& info = m_vehicle->getWheelInfo(wheelIndex);
			info.m_rollInfluence = rollInfluence;
		}
	}

	virtual void SetCoordinateSystem(int rightIndex, int upIndex, int forwardIndex)
	{
		m_vehicle->setCoordinateSystem(rightIndex, upIndex, forwardIndex);
	}

	virtual void SetRayCastMask(short mask)
	{
		m_raycaster->SetRayCastMask(mask);
	}
	virtual short GetRayCastMask() const
	{
		return m_raycaster->GetRayCastMask();
	}
};

class CcdOverlapFilterCallBack : public btOverlapFilterCallback
{
private:
	class CcdPhysicsEnvironment *m_physEnv;
public:
	CcdOverlapFilterCallBack(CcdPhysicsEnvironment *env) :
		m_physEnv(env)
	{
	}
	virtual ~CcdOverlapFilterCallBack()
	{
	}
	/// return true when pairs need collision
	virtual bool needBroadphaseCollision(btBroadphaseProxy *proxy0, btBroadphaseProxy *proxy1) const;
};

CcdDebugDraw::CcdDebugDraw()
	:m_debugMode(0)
{
}
void CcdDebugDraw::drawLine(const btVector3& from, const btVector3& to, const btVector3& color)
{
	if (m_debugMode > 0) {
		KX_RasterizerDrawDebugLine(ToMt(from), ToMt(to), mt::vec4(color.x(), color.y(), color.z(), 1.0f));
	}
}

void CcdDebugDraw::reportErrorWarning(const char *warningString)
{
}

void CcdDebugDraw::drawContactPoint(const btVector3& PointOnB, const btVector3& normalOnB, float distance, int lifeTime, const btVector3& color)
{
	drawLine(PointOnB, PointOnB + normalOnB, color);
	drawSphere(PointOnB, 0.1f, color);
}

void CcdDebugDraw::setDebugMode(int debugMode)
{
	m_debugMode = debugMode;
}

int CcdDebugDraw::getDebugMode() const
{
	return m_debugMode;
}

void CcdDebugDraw::draw3dText(const btVector3& location, const char *textString)
{
}

CcdPhysicsEnvironment::CcdPhysicsEnvironment(PHY_SolverType solverType, bool useDbvtCulling)
	:m_collisionConfiguration(new btSoftBodyRigidBodyCollisionConfiguration()),
	m_broadphase(new btDbvtBroadphase()),
	m_cullingCache(nullptr),
	m_cullingTree(nullptr),
	m_solverMt(new btSequentialImpulseConstraintSolverMt()),
	m_filterCallback(new CcdOverlapFilterCallBack(this)),
	m_ghostPairCallback(new btGhostPairCallback()),
	m_dispatcher(new btCollisionDispatcherMt(m_collisionConfiguration.get())),
	m_numIterations(10),
	m_numTimeSubSteps(1),
	m_ccdMode(0),
	m_solverType(PHY_SOLVER_NONE),
	m_deactivationTime(2.0f),
	m_linearDeactivationThreshold(0.8f),
	m_angularDeactivationThreshold(1.0f),
	m_contactBreakingThreshold(0.02f)
{
	// Initialize the task scheduler used for bullet parallelization.
	btITaskScheduler *scheduler = btGetTBBTaskScheduler();
	const int numThread = scheduler->getMaxNumThreads();
	if (btGetTaskScheduler() != scheduler) {
		scheduler->setNumThreads(numThread);
		btSetTaskScheduler(scheduler);
	}

	for (int i = 0; i < PHY_NUM_RESPONSE; i++) {
		m_triggerCallbacks[i] = nullptr;
	}

	btGImpactCollisionAlgorithm::registerAlgorithm(m_dispatcher.get());

	// avoid any collision in the culling tree
	if (useDbvtCulling) {
		m_cullingCache.reset(new btNullPairCache());
		m_cullingTree.reset(new btDbvtBroadphase(m_cullingCache.get()));
	}

	m_broadphase->getOverlappingPairCache()->setOverlapFilterCallback(m_filterCallback.get());
	m_broadphase->getOverlappingPairCache()->setInternalGhostPairCallback(m_ghostPairCallback.get());

	m_solvers.resize(numThread);
	SetSolverType(solverType);
	
    m_solverPool.reset(new btConstraintSolverPoolMt(m_solvers.data(), numThread));
	m_dynamicsWorld.reset(new btSoftRigidDynamicsWorldMt(m_dispatcher.get(), m_broadphase.get(), m_solverPool.get(), m_solverMt.get(), m_collisionConfiguration.get()));
	m_dynamicsWorld->setInternalTickCallback(&CcdPhysicsEnvironment::StaticSimulationSubtickCallback, this);

	m_dynamicsWorld->setDebugDrawer(&m_debugDrawer);

	SetGravity(0.0f, 0.0f, -9.81f);
}

void CcdPhysicsEnvironment::AddCcdPhysicsController(CcdPhysicsController *ctrl)
{
	// the controller is already added we do nothing
	if (!m_controllers.insert(ctrl).second) {
		return;
	}

	btRigidBody *body = ctrl->GetRigidBody();
	btCollisionObject *obj = ctrl->GetCollisionObject();

	//this m_userPointer is just used for triggers, see CallbackTriggers
	obj->setUserPointer(ctrl);
	if (body) {
		body->setGravity(m_gravity);
		body->setSleepingThresholds(m_linearDeactivationThreshold, m_angularDeactivationThreshold);
		//use explicit group/filter for finer control over collision in bullet => near/radar sensor
		m_dynamicsWorld->addRigidBody(body, ctrl->GetCollisionFilterGroup(), ctrl->GetCollisionFilterMask());

		// Restore constraints in case of physics restore.
		for (unsigned short i = 0, size = ctrl->getNumCcdConstraintRefs(); i < size; ++i) {
			btTypedConstraint *con = ctrl->getCcdConstraintRef(i);
			RestoreConstraint(ctrl, con);
		}

		// Handle potential vehicle constraints
		for (WrapperVehicle *wrapperVehicle : m_wrapperVehicles) {
			if (wrapperVehicle->GetChassis() == ctrl) {
				btRaycastVehicle *vehicle = wrapperVehicle->GetVehicle();
				m_dynamicsWorld->addVehicle(vehicle);
			}
		}
	}
	else {
		if (ctrl->GetSoftBody()) {
			btSoftBody *softBody = ctrl->GetSoftBody();
			m_dynamicsWorld->addSoftBody(softBody);
		}
		else {
			if (obj->getCollisionShape()) {
				m_dynamicsWorld->addCollisionObject(obj, ctrl->GetCollisionFilterGroup(), ctrl->GetCollisionFilterMask());
			}
			if (ctrl->GetCharacterController()) {
				m_dynamicsWorld->addAction(ctrl->GetCharacterController());
			}
		}
	}
	if (obj->isStaticOrKinematicObject()) {
		obj->setActivationState(ISLAND_SLEEPING);
	}

	BLI_assert(obj->getBroadphaseHandle());
}

void CcdPhysicsEnvironment::RemoveConstraint(btTypedConstraint *con, bool free)
{
	CcdConstraint *userData = (CcdConstraint *)con->getUserConstraintPtr();
	if (!userData->GetActive()) {
		return;
	}

	btRigidBody &rbA = con->getRigidBodyA();
	btRigidBody &rbB = con->getRigidBodyB();
	rbA.activate();
	rbB.activate();

	userData->SetActive(false);
	m_dynamicsWorld->removeConstraint(con);

	if (free) {
		if (rbA.getUserPointer()) {
			((CcdPhysicsController *)rbA.getUserPointer())->removeCcdConstraintRef(con);
		}

		if (rbB.getUserPointer()) {
			((CcdPhysicsController *)rbB.getUserPointer())->removeCcdConstraintRef(con);
		}

		/* Since we remove the constraint in the onwer and the target, we can delete it,
		 * KX_ConstraintWrapper keep the constraint id not the pointer, so no problems. */
		delete userData;
		delete con;
	}
}

void CcdPhysicsEnvironment::RemoveVehicle(WrapperVehicle *vehicle, bool free)
{
	m_dynamicsWorld->removeVehicle(vehicle->GetVehicle());
	if (free) {
		CM_ListRemoveIfFound(m_wrapperVehicles, vehicle);
		delete vehicle;
	}
}

void CcdPhysicsEnvironment::RemoveVehicle(CcdPhysicsController *ctrl, bool free)
{
	for (std::vector<WrapperVehicle *>::iterator it = m_wrapperVehicles.begin(); it != m_wrapperVehicles.end(); ) {
		WrapperVehicle *vehicle = *it;
		if (vehicle->GetChassis() == ctrl) {
			m_dynamicsWorld->removeVehicle(vehicle->GetVehicle());
			if (free) {
				it = m_wrapperVehicles.erase(it);
				delete vehicle;
				continue;
			}
		}
		++it;
	}
}

void CcdPhysicsEnvironment::RestoreConstraint(CcdPhysicsController *ctrl, btTypedConstraint *con)
{
	CcdConstraint *userData = (CcdConstraint *)con->getUserConstraintPtr();
	if (userData->GetActive()) {
		return;
	}

	btRigidBody &rbA = con->getRigidBodyA();
	btRigidBody &rbB = con->getRigidBodyB();

	CcdPhysicsController *other = nullptr;

	if (rbA.getUserPointer() && rbB.getUserPointer()) {
		CcdPhysicsController *ctrl0 = (CcdPhysicsController *)rbA.getUserPointer();
		CcdPhysicsController *ctrl1 = (CcdPhysicsController *)rbB.getUserPointer();
		other = (ctrl0 != ctrl) ? ctrl0 : ctrl1;
	}

	BLI_assert(other != nullptr);

	// Avoid add constraint if one of the objects are not available.
	if (IsActiveCcdPhysicsController(other)) {
		userData->SetActive(true);
		m_dynamicsWorld->addConstraint(con, userData->GetDisableCollision());
	}
}

bool CcdPhysicsEnvironment::RemoveCcdPhysicsController(CcdPhysicsController *ctrl, bool freeConstraints)
{
	// if the physics controller is already removed we do nothing
	if (!m_controllers.erase(ctrl)) {
		return false;
	}

	//also remove constraint
	btRigidBody *body = ctrl->GetRigidBody();
	if (body) {
		btBroadphaseProxy *proxy = ctrl->GetCollisionObject()->getBroadphaseHandle();
		btDispatcher *dispatcher = m_dynamicsWorld->getDispatcher();
		btOverlappingPairCache *pairCache = m_dynamicsWorld->getPairCache();

		CleanPairCallback cleanPairs(proxy, pairCache, dispatcher);
		pairCache->processAllOverlappingPairs(&cleanPairs, dispatcher);

		for (int i = ctrl->getNumCcdConstraintRefs() - 1; i >= 0; i--) {
			btTypedConstraint *con = ctrl->getCcdConstraintRef(i);
			RemoveConstraint(con, freeConstraints);
		}
		m_dynamicsWorld->removeRigidBody(ctrl->GetRigidBody());

		// Handle potential vehicle constraints
		RemoveVehicle(ctrl, freeConstraints);
	}
	else {
		//if a softbody
		if (ctrl->GetSoftBody()) {
			m_dynamicsWorld->removeSoftBody(ctrl->GetSoftBody());
		}
		else {
			m_dynamicsWorld->removeCollisionObject(ctrl->GetCollisionObject());

			if (ctrl->GetCharacterController()) {
				m_dynamicsWorld->removeAction(ctrl->GetCharacterController());
			}
		}
	}

	return true;
}

void CcdPhysicsEnvironment::UpdateCcdPhysicsController(CcdPhysicsController *ctrl, btScalar newMass, int newCollisionFlags, short int newCollisionGroup, short int newCollisionMask)
{
	// this function is used when the collisionning group of a controller is changed
	// remove and add the collistioning object
	btRigidBody *body = ctrl->GetRigidBody();
	btSoftBody *softBody = ctrl->GetSoftBody();
	btCollisionObject *obj = ctrl->GetCollisionObject();
	if (obj) {
		btVector3 inertia(0.0, 0.0, 0.0);
		m_dynamicsWorld->removeCollisionObject(obj);
		obj->setCollisionFlags(newCollisionFlags);
		if (body) {
			if (newMass) {
				body->getCollisionShape()->calculateLocalInertia(newMass, inertia);
			}
			body->setMassProps(newMass, inertia * ctrl->GetInertiaFactor());
			m_dynamicsWorld->addRigidBody(body, newCollisionGroup, newCollisionMask);
		}
		else if (softBody) {
			m_dynamicsWorld->addSoftBody(softBody);
		}
		else {
			m_dynamicsWorld->addCollisionObject(obj, newCollisionGroup, newCollisionMask);
		}
	}
	// to avoid nasty interaction, we must update the property of the controller as well
	ctrl->m_cci.m_mass = newMass;
	ctrl->m_cci.m_collisionFilterGroup = newCollisionGroup;
	ctrl->m_cci.m_collisionFilterMask = newCollisionMask;
	ctrl->m_cci.m_collisionFlags = newCollisionFlags;
}

void CcdPhysicsEnvironment::RefreshCcdPhysicsController(CcdPhysicsController *ctrl)
{
	btCollisionObject *obj = ctrl->GetCollisionObject();
	if (obj) {
		btBroadphaseProxy *proxy = obj->getBroadphaseHandle();
		if (proxy) {
			m_dynamicsWorld->getPairCache()->cleanProxyFromPairs(proxy, m_dynamicsWorld->getDispatcher());
		}
	}
}

bool CcdPhysicsEnvironment::IsActiveCcdPhysicsController(CcdPhysicsController *ctrl)
{
	return (m_controllers.find(ctrl) != m_controllers.end());
}

void CcdPhysicsEnvironment::AddCcdGraphicController(CcdGraphicController *ctrl)
{
	if (m_cullingTree && !ctrl->GetBroadphaseHandle()) {
		btVector3 minAabb;
		btVector3 maxAabb;
		ctrl->GetAabb(minAabb, maxAabb);

		ctrl->SetBroadphaseHandle(m_cullingTree->createProxy(
									  minAabb,
									  maxAabb,
									  INVALID_SHAPE_PROXYTYPE, // this parameter is not used
									  ctrl,
									  0, // this object does not collision with anything
									  0,
									  nullptr // dispatcher => this parameter is not used
									  ));

		BLI_assert(ctrl->GetBroadphaseHandle());
	}
}

void CcdPhysicsEnvironment::RemoveCcdGraphicController(CcdGraphicController *ctrl)
{
	if (m_cullingTree) {
		btBroadphaseProxy *bp = ctrl->GetBroadphaseHandle();
		if (bp) {
			m_cullingTree->destroyProxy(bp, nullptr);
			ctrl->SetBroadphaseHandle(nullptr);
		}
	}
}

void CcdPhysicsEnvironment::UpdateCcdPhysicsControllerShape(CcdShapeConstructionInfo *shapeInfo)
{
	for (CcdPhysicsController *ctrl : m_controllers) {
		if (ctrl->GetShapeInfo() != shapeInfo) {
			continue;
		}

		ctrl->ReplaceControllerShape(nullptr);
		RefreshCcdPhysicsController(ctrl);
	}
}

void CcdPhysicsEnvironment::DebugDrawWorld()
{
	m_dynamicsWorld->debugDrawWorld();
}

void CcdPhysicsEnvironment::StaticSimulationSubtickCallback(btDynamicsWorld *world, btScalar timeStep)
{
	// Get the pointer to the CcdPhysicsEnvironment associated with this Bullet world.
	CcdPhysicsEnvironment *this_ = static_cast<CcdPhysicsEnvironment *>(world->getWorldUserInfo());
	this_->SimulationSubtickCallback(timeStep);
}

void CcdPhysicsEnvironment::SimulationSubtickCallback(btScalar timeStep)
{
	std::set<CcdPhysicsController *>::iterator it;

	for (it = m_controllers.begin(); it != m_controllers.end(); it++) {
		(*it)->SimulationTick(timeStep);
	}
}

bool CcdPhysicsEnvironment::ProceedDeltaTime(double curTime, float timeStep, float interval)
{
	std::set<CcdPhysicsController *>::iterator it;
	int i;

	// Update Bullet global variables.
	gDeactivationTime = m_deactivationTime;
	gContactBreakingThreshold = m_contactBreakingThreshold;

	for (it = m_controllers.begin(); it != m_controllers.end(); it++) {
		(*it)->SynchronizeMotionStates(timeStep);
	}

	float subStep = timeStep / float(m_numTimeSubSteps);
	i = m_dynamicsWorld->stepSimulation(interval, 25, subStep);//perform always a full simulation step
//uncomment next line to see where Bullet spend its time (printf in console)
//CProfileManager::dumpAll();

	ProcessFhSprings(curTime, i * subStep);

	for (it = m_controllers.begin(); it != m_controllers.end(); it++) {
		(*it)->SynchronizeMotionStates(timeStep);
	}

	for (i = 0; i < m_wrapperVehicles.size(); i++) {
		WrapperVehicle *veh = m_wrapperVehicles[i];
		veh->SyncWheels();
	}

	CallbackTriggers();

	return true;
}

class ClosestRayResultCallbackNotMe : public btCollisionWorld::ClosestRayResultCallback
{
	btCollisionObject *m_owner;
	btCollisionObject *m_parent;

public:
	ClosestRayResultCallbackNotMe(const btVector3& rayFromWorld, const btVector3& rayToWorld, btCollisionObject *owner, btCollisionObject *parent)
		:btCollisionWorld::ClosestRayResultCallback(rayFromWorld, rayToWorld),
		m_owner(owner),
		m_parent(parent)
	{
	}

	virtual bool needsCollision(btBroadphaseProxy *proxy0) const
	{
		//don't collide with self
		if (proxy0->m_clientObject == m_owner) {
			return false;
		}

		if (proxy0->m_clientObject == m_parent) {
			return false;
		}

		return btCollisionWorld::ClosestRayResultCallback::needsCollision(proxy0);
	}
};

void CcdPhysicsEnvironment::ProcessFhSprings(double curTime, float interval)
{
	std::set<CcdPhysicsController *>::iterator it;

	const float step = interval * KX_GetActiveEngine()->GetTicRate();

	for (it = m_controllers.begin(); it != m_controllers.end(); it++) {
		CcdPhysicsController *ctrl = (*it);
		btRigidBody *body = ctrl->GetRigidBody();

		if (body && (ctrl->GetConstructionInfo().m_do_fh || ctrl->GetConstructionInfo().m_do_rot_fh)) {
			//re-implement SM_FhObject.cpp using btCollisionWorld::rayTest and info from ctrl->getConstructionInfo()
			//send a ray from {0.0, 0.0, 0.0} towards {0.0, 0.0, -10.0}, in local coordinates
			CcdPhysicsController *parentCtrl = ctrl->GetParentRoot();
			btRigidBody *parentBody = parentCtrl ? parentCtrl->GetRigidBody() : nullptr;
			btRigidBody *cl_object = parentBody ? parentBody : body;

			if (body->isStaticOrKinematicObject()) {
				continue;
			}

			btVector3 rayDirLocal(0.0f, 0.0f, -10.0f);

			//m_dynamicsWorld
			//ctrl->GetRigidBody();
			btVector3 rayFromWorld = body->getCenterOfMassPosition();
			//btVector3	rayToWorld = rayFromWorld + body->getCenterOfMassTransform().getBasis() * rayDirLocal;
			//ray always points down the z axis in world space...
			btVector3 rayToWorld = rayFromWorld + rayDirLocal;

			ClosestRayResultCallbackNotMe resultCallback(rayFromWorld, rayToWorld, body, parentBody);

			m_dynamicsWorld->rayTest(rayFromWorld, rayToWorld, resultCallback);
			if (resultCallback.hasHit()) {
				//we hit this one: resultCallback.m_collisionObject;
				CcdPhysicsController *controller = static_cast<CcdPhysicsController *>(resultCallback.m_collisionObject->getUserPointer());

				if (controller) {
					if (controller->GetConstructionInfo().m_fh_distance < SIMD_EPSILON) {
						continue;
					}

					btRigidBody *hit_object = controller->GetRigidBody();
					if (!hit_object) {
						continue;
					}

					CcdConstructionInfo& hitObjShapeProps = controller->GetConstructionInfo();

					float distance = resultCallback.m_closestHitFraction * rayDirLocal.length() - ctrl->GetConstructionInfo().m_radius;
					if (distance >= hitObjShapeProps.m_fh_distance) {
						continue;
					}

					//btVector3 ray_dir = cl_object->getCenterOfMassTransform().getBasis()* rayDirLocal.Normalized();
					btVector3 ray_dir = rayDirLocal.normalized();
					btVector3 normal = resultCallback.m_hitNormalWorld;
					normal.normalize();

					if (ctrl->GetConstructionInfo().m_do_fh) {
						btVector3 lspot = cl_object->getCenterOfMassPosition() +
						                  rayDirLocal * resultCallback.m_closestHitFraction;

						lspot -= hit_object->getCenterOfMassPosition();
						btVector3 rel_vel = cl_object->getLinearVelocity() - hit_object->getVelocityInLocalPoint(lspot);
						btScalar rel_vel_ray = ray_dir.dot(rel_vel);
						btScalar spring_extent = 1.0f - distance / hitObjShapeProps.m_fh_distance;

						btScalar i_spring = spring_extent * hitObjShapeProps.m_fh_spring;
						btScalar i_damp =   rel_vel_ray * hitObjShapeProps.m_fh_damping;

						cl_object->setLinearVelocity(cl_object->getLinearVelocity() + (-(i_spring + i_damp) * ray_dir) * step);
						if (hitObjShapeProps.m_fh_normal) {
							cl_object->setLinearVelocity(cl_object->getLinearVelocity() + (i_spring + i_damp) * (normal - normal.dot(ray_dir) * ray_dir) * step);
						}

						btVector3 lateral = rel_vel - rel_vel_ray * ray_dir;

						if (ctrl->GetConstructionInfo().m_do_anisotropic) {
							//Bullet basis contains no scaling/shear etc.
							const btMatrix3x3& lcs = cl_object->getCenterOfMassTransform().getBasis();
							btVector3 loc_lateral = lateral * lcs;
							const btVector3& friction_scaling = cl_object->getAnisotropicFriction();
							loc_lateral *= friction_scaling;
							lateral = lcs * loc_lateral;
						}

						btScalar rel_vel_lateral = lateral.length();

						if (rel_vel_lateral > SIMD_EPSILON) {
							btScalar friction_factor = hit_object->getFriction();//cl_object->getFriction();

							btScalar max_friction = friction_factor * btMax(btScalar(0.0), i_spring);

							btScalar rel_mom_lateral = rel_vel_lateral / cl_object->getInvMass();

							btVector3 friction = (rel_mom_lateral > max_friction) ?
							                     -lateral * (max_friction / rel_vel_lateral) :
							                     -lateral;

							cl_object->applyCentralImpulse(friction * step);
						}
					}


					if (ctrl->GetConstructionInfo().m_do_rot_fh) {
						btVector3 up2 = cl_object->getWorldTransform().getBasis().getColumn(2);

						btVector3 t_spring = up2.cross(normal) * hitObjShapeProps.m_fh_spring;
						btVector3 ang_vel = cl_object->getAngularVelocity();

						// only rotations that tilt relative to the normal are damped
						ang_vel -= ang_vel.dot(normal) * normal;

						btVector3 t_damp = ang_vel * hitObjShapeProps.m_fh_damping;

						cl_object->setAngularVelocity(cl_object->getAngularVelocity() + (t_spring - t_damp) * step);
					}
				}
			}
		}
	}
}

int CcdPhysicsEnvironment::GetDebugMode() const
{
	return m_debugDrawer.getDebugMode();
}

void CcdPhysicsEnvironment::SetDebugMode(int debugMode)
{
	m_debugDrawer.setDebugMode(debugMode);
}

void CcdPhysicsEnvironment::SetNumIterations(int numIter)
{
	m_numIterations = numIter;
}
void CcdPhysicsEnvironment::SetDeactivationTime(float dTime)
{
	m_deactivationTime = dTime;
}
void CcdPhysicsEnvironment::SetDeactivationLinearTreshold(float linTresh)
{
	m_linearDeactivationThreshold = linTresh;

	// Update from all controllers.
	for (CcdPhysicsController *ctrl : m_controllers) {
		if (ctrl->GetRigidBody()) {
			ctrl->GetRigidBody()->setSleepingThresholds(m_linearDeactivationThreshold, m_angularDeactivationThreshold);
		}
	}
}
void CcdPhysicsEnvironment::SetDeactivationAngularTreshold(float angTresh)
{
	m_angularDeactivationThreshold = angTresh;

	// Update from all controllers.
	for (std::set<CcdPhysicsController *>::iterator it = m_controllers.begin(); it != m_controllers.end(); it++) {
		if ((*it)->GetRigidBody()) {
			(*it)->GetRigidBody()->setSleepingThresholds(m_linearDeactivationThreshold, m_angularDeactivationThreshold);
		}
	}
}

void CcdPhysicsEnvironment::SetContactBreakingTreshold(float contactBreakingTreshold)
{
	m_contactBreakingThreshold = contactBreakingTreshold;
}

void CcdPhysicsEnvironment::SetCcdMode(int ccdMode)
{
	m_ccdMode = ccdMode;
}

void CcdPhysicsEnvironment::SetSolverSorConstant(float sor)
{
	m_dynamicsWorld->getSolverInfo().m_sor = sor;
}

void CcdPhysicsEnvironment::SetSolverTau(float tau)
{
	m_dynamicsWorld->getSolverInfo().m_tau = tau;
}
void CcdPhysicsEnvironment::SetSolverDamping(float damping)
{
	m_dynamicsWorld->getSolverInfo().m_damping = damping;
}

void CcdPhysicsEnvironment::SetLinearAirDamping(float damping)
{
	//gLinearAirDamping = damping;
}

void CcdPhysicsEnvironment::SetUseEpa(bool epa)
{
	//gUseEpa = epa;
}

void CcdPhysicsEnvironment::SetSolverType(PHY_SolverType solverType)
{
	if (m_solverType == solverType) {
		return;
	}

	for (unsigned short i = 0, size = m_solvers.size(); i < size; ++i) {
		switch (solverType) {
			case PHY_SOLVER_SEQUENTIAL:
			{
				m_solvers[i] = new btSequentialImpulseConstraintSolver();
				break;
			}
			case PHY_SOLVER_NNCG:
			{
				m_solvers[i] = new btNNCGConstraintSolver();
				break;
			}
			case PHY_SOLVER_MLCP_DANTZIG:
			{
				m_solvers[i] = new btMLCPSolver(new btDantzigSolver());
				break;
			}
			case PHY_SOLVER_MLCP_LEMKE:
			{
				m_solvers[i] = new btMLCPSolver(new btLemkeSolver());
				break;
			}
			default:
			{
				BLI_assert(false);
			}
		}
	}

	m_solverType = solverType;
}

mt::vec3 CcdPhysicsEnvironment::GetGravity() const
{
	return ToMt(m_dynamicsWorld->getGravity());
}

void CcdPhysicsEnvironment::SetGravity(float x, float y, float z)
{
	m_gravity = btVector3(x, y, z);
	m_dynamicsWorld->setGravity(m_gravity);
	m_dynamicsWorld->getWorldInfo().m_gravity.setValue(x, y, z);
}

static int gConstraintUid = 1;

void CcdPhysicsEnvironment::RemoveConstraintById(int constraintId, bool free)
{
	// For soft body constraints
	if (constraintId == 0) {
		return;
	}

	int i;
	int numConstraints = m_dynamicsWorld->getNumConstraints();
	for (i = 0; i < numConstraints; i++) {
		btTypedConstraint *constraint = m_dynamicsWorld->getConstraint(i);
		if (constraint->getUserConstraintId() == constraintId) {
			RemoveConstraint(constraint, free);
			break;
		}
	}

	WrapperVehicle *vehicle = static_cast<WrapperVehicle *>(GetVehicleConstraint(constraintId));
	if (vehicle) {
		RemoveVehicle(vehicle, free);
	}
}

struct  FilterClosestRayResultCallback : public btCollisionWorld::ClosestRayResultCallback {
	PHY_IRayCastFilterCallback& m_phyRayFilter;
	int m_hitChildIndex;
	int m_hitTriangleIndex;

	FilterClosestRayResultCallback(PHY_IRayCastFilterCallback& phyRayFilter, const btVector3& rayFrom, const btVector3& rayTo)
		:btCollisionWorld::ClosestRayResultCallback(rayFrom, rayTo),
		m_phyRayFilter(phyRayFilter),
		m_hitChildIndex(-1),
		m_hitTriangleIndex(0)
	{
	}

	virtual ~FilterClosestRayResultCallback()
	{
	}

	virtual bool needsCollision(btBroadphaseProxy *proxy0) const
	{
		if (!(proxy0->m_collisionFilterGroup & m_collisionFilterMask)) {
			return false;
		}
		if (!(m_collisionFilterGroup & proxy0->m_collisionFilterMask)) {
			return false;
		}
		btCollisionObject *object = (btCollisionObject *)proxy0->m_clientObject;
		CcdPhysicsController *phyCtrl = static_cast<CcdPhysicsController *>(object->getUserPointer());
		if (phyCtrl == m_phyRayFilter.m_ignoreController) {
			return false;
		}
		return m_phyRayFilter.needBroadphaseRayCast(phyCtrl);
	}

	virtual btScalar addSingleResult(btCollisionWorld::LocalRayResult& rayResult, bool normalInWorldSpace)
	{
		m_hitChildIndex = rayResult.m_childIndex;
		if (rayResult.m_localShapeInfo) {
			m_hitTriangleIndex = rayResult.m_localShapeInfo->m_triangleIndex;
		}
		else {
			m_hitTriangleIndex = 0;
		}
		return ClosestRayResultCallback::addSingleResult(rayResult, normalInWorldSpace);
	}
};

static bool GetHitTriangle(const btCollisionShape *shape, CcdShapeConstructionInfo *shapeInfo, int hitTriangleIndex, btVector3 triangle[])
{
	// this code is copied from Bullet
	const unsigned char *vertexbase;
	int numverts;
	PHY_ScalarType type;
	int stride;
	const unsigned char *indexbase;
	int indexstride;
	int numfaces;
	PHY_ScalarType indicestype;
	btStridingMeshInterface *meshInterface = shapeInfo->GetMeshInterface();

	if (!meshInterface) {
		return false;
	}

	meshInterface->getLockedReadOnlyVertexIndexBase(
		&vertexbase,
		numverts,
		type,
		stride,
		&indexbase,
		indexstride,
		numfaces,
		indicestype,
		0);

	unsigned int *gfxbase = (unsigned int *)(indexbase + hitTriangleIndex * indexstride);
	const btVector3& meshScaling = shape->getLocalScaling();
	for (int j = 2; j >= 0; j--) {
		int graphicsindex = (indicestype == PHY_SHORT) ? ((unsigned short *)gfxbase)[j] : gfxbase[j];

		btScalar *graphicsbase = (btScalar *)(vertexbase + graphicsindex * stride);

		triangle[j] = btVector3(graphicsbase[0] * meshScaling.getX(), graphicsbase[1] * meshScaling.getY(), graphicsbase[2] * meshScaling.getZ());
	}
	meshInterface->unLockReadOnlyVertexBase(0);
	return true;
}

PHY_IPhysicsController *CcdPhysicsEnvironment::RayTest(PHY_IRayCastFilterCallback &filterCallback, float fromX, float fromY, float fromZ, float toX, float toY, float toZ)
{
	btVector3 rayFrom(fromX, fromY, fromZ);
	btVector3 rayTo(toX, toY, toZ);

	btVector3 hitPointWorld, normalWorld;

	//Either Ray Cast with or without filtering

	FilterClosestRayResultCallback rayCallback(filterCallback, rayFrom, rayTo);
	PHY_RayCastResult result;

	// don't collision with sensor object
	rayCallback.m_collisionFilterMask = CcdConstructionInfo::AllFilter ^ CcdConstructionInfo::SensorFilter;
	// use faster (less accurate) ray callback, works better with 0 collision margins
	rayCallback.m_flags |= btTriangleRaycastCallback::kF_UseSubSimplexConvexCastRaytest;

	m_dynamicsWorld->rayTest(rayFrom, rayTo, rayCallback);
	if (rayCallback.hasHit()) {
		const btCollisionObject *object = rayCallback.m_collisionObject;
		const btCollisionShape *shape = object->getCollisionShape();

		CcdPhysicsController *controller = static_cast<CcdPhysicsController *>(rayCallback.m_collisionObject->getUserPointer());
		result.m_controller = controller;
		result.m_hitPoint = ToMt(rayCallback.m_hitPointWorld);

		if (shape) {
			if (shape->isCompound()) {
				const btCompoundShape *compoundShape = static_cast<const btCompoundShape *>(shape);
				shape = compoundShape->getChildShape(rayCallback.m_hitChildIndex);
			}

			CcdShapeConstructionInfo *shapeInfo = static_cast<CcdShapeConstructionInfo *>(shape->getUserPointer());
			if (shapeInfo && rayCallback.m_hitTriangleIndex < shapeInfo->m_polygonIndexArray.size()) {
				// save original collision shape triangle for soft body
				const int hitTriangleIndex = rayCallback.m_hitTriangleIndex;

				result.m_meshObject = shapeInfo->GetMesh();
				if (shape->isSoftBody()) {
					// soft body using different face numbering because of randomization
					// hopefully we have stored the original face number in m_tag
					const btSoftBody *softBody = static_cast<const btSoftBody *>(object);
					if (softBody->m_faces[hitTriangleIndex].m_tag != 0) {
						rayCallback.m_hitTriangleIndex = (int)((uintptr_t)(softBody->m_faces[hitTriangleIndex].m_tag) - 1);
					}
				}
				// retrieve the original mesh polygon (in case of quad->tri conversion)
				result.m_polygon = shapeInfo->m_polygonIndexArray[rayCallback.m_hitTriangleIndex];
				// hit triangle in world coordinate, for face normal and UV coordinate
				btVector3 triangle[3];
				bool triangleOK = false;
				if (filterCallback.m_faceUV && (3 * rayCallback.m_hitTriangleIndex) < shapeInfo->m_triFaceUVcoArray.size()) {
					// interpolate the UV coordinate of the hit point
					CcdShapeConstructionInfo::UVco *uvCo = &shapeInfo->m_triFaceUVcoArray[3 * rayCallback.m_hitTriangleIndex];
					// 1. get the 3 coordinate of the triangle in world space
					btVector3 v1, v2, v3;
					if (shape->isSoftBody()) {
						// soft body give points directly in world coordinate
						const btSoftBody *softBody = static_cast<const btSoftBody *>(object);
						v1 = softBody->m_faces[hitTriangleIndex].m_n[0]->m_x;
						v2 = softBody->m_faces[hitTriangleIndex].m_n[1]->m_x;
						v3 = softBody->m_faces[hitTriangleIndex].m_n[2]->m_x;
					}
					else {
						// for rigid body we must apply the world transform
						triangleOK = GetHitTriangle(shape, shapeInfo, hitTriangleIndex, triangle);
						if (!triangleOK) {
							// if we cannot get the triangle, no use to continue
							goto SKIP_UV_NORMAL;
						}
						const btTransform& trans = object->getWorldTransform();
						v1 = trans(triangle[0]);
						v2 = trans(triangle[1]);
						v3 = trans(triangle[2]);
					}
					// 2. compute barycentric coordinate of the hit point
					btVector3 v = v2 - v1;
					btVector3 w = v3 - v1;
					btVector3 u = v.cross(w);
					btScalar A = u.length();

					v = v2 - rayCallback.m_hitPointWorld;
					w = v3 - rayCallback.m_hitPointWorld;
					u = v.cross(w);
					btScalar A1 = u.length();

					v = rayCallback.m_hitPointWorld - v1;
					w = v3 - v1;
					u = v.cross(w);
					btScalar A2 = u.length();

					btVector3 baryCo;
					baryCo.setX(A1 / A);
					baryCo.setY(A2 / A);
					baryCo.setZ(1.0f - baryCo.getX() - baryCo.getY());
					// 3. compute UV coordinate
					result.m_hitUV[0] = baryCo.getX() * uvCo[0].uv[0] + baryCo.getY() * uvCo[1].uv[0] + baryCo.getZ() * uvCo[2].uv[0];
					result.m_hitUV[1] = baryCo.getX() * uvCo[0].uv[1] + baryCo.getY() * uvCo[1].uv[1] + baryCo.getZ() * uvCo[2].uv[1];
					result.m_hitUVOK = 1;
				}

				// Bullet returns the normal from "outside".
				// If the user requests the real normal, compute it now
				if (filterCallback.m_faceNormal) {
					if (shape->isSoftBody()) {
						// we can get the real normal directly from the body
						const btSoftBody *softBody = static_cast<const btSoftBody *>(rayCallback.m_collisionObject);
						rayCallback.m_hitNormalWorld = softBody->m_faces[hitTriangleIndex].m_normal;
					}
					else {
						if (!triangleOK) {
							triangleOK = GetHitTriangle(shape, shapeInfo, hitTriangleIndex, triangle);
						}
						if (triangleOK) {
							btVector3 triangleNormal;
							triangleNormal = (triangle[1] - triangle[0]).cross(triangle[2] - triangle[0]);
							rayCallback.m_hitNormalWorld = rayCallback.m_collisionObject->getWorldTransform().getBasis() * triangleNormal;
						}
					}
				}
SKIP_UV_NORMAL:
				;
			}
		}
		if (rayCallback.m_hitNormalWorld.length2() > (SIMD_EPSILON * SIMD_EPSILON)) {
			rayCallback.m_hitNormalWorld.normalize();
		}
		else {
			rayCallback.m_hitNormalWorld.setValue(1.0f, 0.0f, 0.0f);
		}

		result.m_hitNormal = ToMt(rayCallback.m_hitNormalWorld);
		filterCallback.reportHit(&result);
	}

	return result.m_controller;
}

// Handles occlusion culling.
// The implementation is based on the CDTestFramework
struct OcclusionBuffer {
	struct WriteOCL {
		static inline bool Process(btScalar &q, btScalar v)
		{
			if (q < v) {
				q = v;
			}
			return false;
		}
		static inline void Occlusion(bool &flag)
		{
			flag = true;
		}
	};

	struct QueryOCL {
		static inline bool Process(btScalar &q, btScalar v)
		{
			return (q <= v);
		}
		static inline void Occlusion(bool &flag)
		{
		}
	};

	btScalar *m_buffer;
	size_t m_bufferSize;
	bool m_initialized;
	bool m_occlusion;
	int m_sizes[2];
	btScalar m_scales[2];
	btScalar m_offsets[2];
	btScalar m_wtc[16]; // world to clip transform
	btScalar m_mtc[16]; // model to clip transform
	// constructor: size=largest dimension of the buffer.
	// Buffer size depends on aspect ratio
	OcclusionBuffer()
	{
		m_initialized = false;
		m_occlusion = false;
		m_buffer = nullptr;
		m_bufferSize = 0;
	}
	// multiplication of column major matrices: m = m1 * m2
	template<typename T1, typename T2>
	void CMmat4mul(btScalar *m, const T1 *m1, const T2 *m2)
	{
		m[0] = btScalar(m1[0] * m2[0] + m1[4] * m2[1] + m1[8] * m2[2] + m1[12] * m2[3]);
		m[1] = btScalar(m1[1] * m2[0] + m1[5] * m2[1] + m1[9] * m2[2] + m1[13] * m2[3]);
		m[2] = btScalar(m1[2] * m2[0] + m1[6] * m2[1] + m1[10] * m2[2] + m1[14] * m2[3]);
		m[3] = btScalar(m1[3] * m2[0] + m1[7] * m2[1] + m1[11] * m2[2] + m1[15] * m2[3]);

		m[4] = btScalar(m1[0] * m2[4] + m1[4] * m2[5] + m1[8] * m2[6] + m1[12] * m2[7]);
		m[5] = btScalar(m1[1] * m2[4] + m1[5] * m2[5] + m1[9] * m2[6] + m1[13] * m2[7]);
		m[6] = btScalar(m1[2] * m2[4] + m1[6] * m2[5] + m1[10] * m2[6] + m1[14] * m2[7]);
		m[7] = btScalar(m1[3] * m2[4] + m1[7] * m2[5] + m1[11] * m2[6] + m1[15] * m2[7]);

		m[8] = btScalar(m1[0] * m2[8] + m1[4] * m2[9] + m1[8] * m2[10] + m1[12] * m2[11]);
		m[9] = btScalar(m1[1] * m2[8] + m1[5] * m2[9] + m1[9] * m2[10] + m1[13] * m2[11]);
		m[10] = btScalar(m1[2] * m2[8] + m1[6] * m2[9] + m1[10] * m2[10] + m1[14] * m2[11]);
		m[11] = btScalar(m1[3] * m2[8] + m1[7] * m2[9] + m1[11] * m2[10] + m1[15] * m2[11]);

		m[12] = btScalar(m1[0] * m2[12] + m1[4] * m2[13] + m1[8] * m2[14] + m1[12] * m2[15]);
		m[13] = btScalar(m1[1] * m2[12] + m1[5] * m2[13] + m1[9] * m2[14] + m1[13] * m2[15]);
		m[14] = btScalar(m1[2] * m2[12] + m1[6] * m2[13] + m1[10] * m2[14] + m1[14] * m2[15]);
		m[15] = btScalar(m1[3] * m2[12] + m1[7] * m2[13] + m1[11] * m2[14] + m1[15] * m2[15]);
	}

	void setup(int size, const int *view, float mat[16])
	{
		m_initialized = false;
		m_occlusion = false;
		// compute the size of the buffer
		int maxsize = (view[2] > view[3]) ? view[2] : view[3];
		BLI_assert(maxsize > 0);
		double ratio = 1.0 / (2 * maxsize);
		// ensure even number
		m_sizes[0] = 2 * ((int)(size * view[2] * ratio + 0.5));
		m_sizes[1] = 2 * ((int)(size * view[3] * ratio + 0.5));
		m_scales[0] = btScalar(m_sizes[0] / 2);
		m_scales[1] = btScalar(m_sizes[1] / 2);
		m_offsets[0] = m_scales[0] + 0.5f;
		m_offsets[1] = m_scales[1] + 0.5f;
		// prepare matrix
		// at this time of the rendering, the modelview matrix is the
		// world to camera transformation and the projection matrix is
		// camera to clip transformation. combine both so that
		for (unsigned short i = 0; i < 16; i++) {
			m_wtc[i] = btScalar(mat[i]);
		}
	}

	void initialize()
	{
		size_t newsize = (m_sizes[0] * m_sizes[1]) * sizeof(btScalar);
		if (m_buffer) {
			// see if we can reuse
			if (newsize > m_bufferSize) {
				free(m_buffer);
				m_buffer = nullptr;
				m_bufferSize = 0;
			}
		}
		if (!m_buffer) {
			m_buffer = (btScalar *)calloc(1, newsize);
			m_bufferSize = newsize;
		}
		else {
			// buffer exists already, just clears it
			memset(m_buffer, 0, newsize);
		}
		// memory allocate must succeed
		BLI_assert(m_buffer != nullptr);
		m_initialized = true;
		m_occlusion = false;
	}

	void SetModelMatrix(float *fl)
	{
		CMmat4mul(m_mtc, m_wtc, fl);
		if (!m_initialized) {
			initialize();
		}
	}

	// transform a segment in world coordinate to clip coordinate
	void transformW(const btVector3 &x, btVector4 &t)
	{
		t[0] = x[0] * m_wtc[0] + x[1] * m_wtc[4] + x[2] * m_wtc[8] + m_wtc[12];
		t[1] = x[0] * m_wtc[1] + x[1] * m_wtc[5] + x[2] * m_wtc[9] + m_wtc[13];
		t[2] = x[0] * m_wtc[2] + x[1] * m_wtc[6] + x[2] * m_wtc[10] + m_wtc[14];
		t[3] = x[0] * m_wtc[3] + x[1] * m_wtc[7] + x[2] * m_wtc[11] + m_wtc[15];
	}

	void transformM(const float *x, btVector4 &t)
	{
		t[0] = x[0] * m_mtc[0] + x[1] * m_mtc[4] + x[2] * m_mtc[8] + m_mtc[12];
		t[1] = x[0] * m_mtc[1] + x[1] * m_mtc[5] + x[2] * m_mtc[9] + m_mtc[13];
		t[2] = x[0] * m_mtc[2] + x[1] * m_mtc[6] + x[2] * m_mtc[10] + m_mtc[14];
		t[3] = x[0] * m_mtc[3] + x[1] * m_mtc[7] + x[2] * m_mtc[11] + m_mtc[15];
	}
	// convert polygon to device coordinates
	static bool project(btVector4 *p, int n)
	{
		for (int i = 0; i < n; ++i) {
			p[i][2] = 1 / p[i][3];
			p[i][0] *= p[i][2];
			p[i][1] *= p[i][2];
		}
		return true;
	}
	// pi: closed polygon in clip coordinate, NP = number of segments
	// po: same polygon with clipped segments removed
	template <const int NP>
	static int clip(const btVector4 *pi, btVector4 *po)
	{
		btScalar s[2 * NP];
		btVector4 pn[2 * NP];
		int i, j, m, n, ni;
		// deal with near clipping
		for (i = 0, m = 0; i < NP; ++i) {
			s[i] = pi[i][2] + pi[i][3];
			if (s[i] < 0) {
				m += 1 << i;
			}
		}
		if (m == ((1 << NP) - 1)) {
			return 0;
		}
		if (m != 0) {
			for (i = NP - 1, j = 0, n = 0; j < NP; i = j++) {
				const btVector4 &a = pi[i];
				const btVector4 &b = pi[j];
				const btScalar t = s[i] / (a[3] + a[2] - b[3] - b[2]);
				if ((t > 0) && (t < 1)) {
					pn[n][0] = a[0] + (b[0] - a[0]) * t;
					pn[n][1] = a[1] + (b[1] - a[1]) * t;
					pn[n][2] = a[2] + (b[2] - a[2]) * t;
					pn[n][3] = a[3] + (b[3] - a[3]) * t;
					++n;
				}
				if (s[j] > 0) {
					pn[n++] = b;
				}
			}
			// ready to test far clipping, start from the modified polygon
			pi = pn;
			ni = n;
		}
		else {
			// no clipping on the near plane, keep same vector
			ni = NP;
		}
		// now deal with far clipping
		for (i = 0, m = 0; i < ni; ++i) {
			s[i] = pi[i][2] - pi[i][3];
			if (s[i] > 0) {
				m += 1 << i;
			}
		}
		if (m == ((1 << ni) - 1)) {
			return 0;
		}
		if (m != 0) {
			for (i = ni - 1, j = 0, n = 0; j < ni; i = j++) {
				const btVector4 &a = pi[i];
				const btVector4 &b = pi[j];
				const btScalar t = s[i] / (a[2] - a[3] - b[2] + b[3]);
				if ((t > 0) && (t < 1)) {
					po[n][0] = a[0] + (b[0] - a[0]) * t;
					po[n][1] = a[1] + (b[1] - a[1]) * t;
					po[n][2] = a[2] + (b[2] - a[2]) * t;
					po[n][3] = a[3] + (b[3] - a[3]) * t;
					++n;
				}
				if (s[j] < 0) {
					po[n++] = b;
				}
			}
			return n;
		}
		for (int i = 0; i < ni; ++i) {
			po[i] = pi[i];
		}
		return ni;
	}
	// write or check a triangle to buffer. a,b,c in device coordinates (-1,+1)
	template <typename POLICY>
	inline bool draw(const btVector4 &a,
	                 const btVector4 &b,
	                 const btVector4 &c,
	                 const float face,
	                 const btScalar minarea)
	{
		const btScalar a2 = btCross(b - a, c - a)[2];
		if ((face * a2) < 0.0f || btFabs(a2) < minarea) {
			return false;
		}
		// further down we are normally going to write to the Zbuffer, mark it so
		POLICY::Occlusion(m_occlusion);

		int x[3], y[3], ib = 1, ic = 2;
		btScalar z[3];
		x[0] = (int)(a.x() * m_scales[0] + m_offsets[0]);
		y[0] = (int)(a.y() * m_scales[1] + m_offsets[1]);
		z[0] = a.z();
		if (a2 < 0.f) {
			// negative aire is possible with double face => must
			// change the order of b and c otherwise the algorithm doesn't work
			ib = 2;
			ic = 1;
		}
		x[ib] = (int)(b.x() * m_scales[0] + m_offsets[0]);
		x[ic] = (int)(c.x() * m_scales[0] + m_offsets[0]);
		y[ib] = (int)(b.y() * m_scales[1] + m_offsets[1]);
		y[ic] = (int)(c.y() * m_scales[1] + m_offsets[1]);
		z[ib] = b.z();
		z[ic] = c.z();
		const int mix = btMax(0, btMin(x[0], btMin(x[1], x[2])));
		const int mxx = btMin(m_sizes[0], 1 + btMax(x[0], btMax(x[1], x[2])));
		const int miy = btMax(0, btMin(y[0], btMin(y[1], y[2])));
		const int mxy = btMin(m_sizes[1], 1 + btMax(y[0], btMax(y[1], y[2])));
		const int width = mxx - mix;
		const int height = mxy - miy;
		if ((width * height) <= 1) {
			// degenerated in at most one single pixel
			btScalar *scan = &m_buffer[miy * m_sizes[0] + mix];
			// use for loop to detect the case where width or height == 0
			for (int iy = miy; iy < mxy; ++iy) {
				for (int ix = mix; ix < mxx; ++ix) {
					if (POLICY::Process(*scan, z[0])) {
						return true;
					}
					if (POLICY::Process(*scan, z[1])) {
						return true;
					}
					if (POLICY::Process(*scan, z[2])) {
						return true;
					}
				}
			}
		}
		else if (width == 1) {
			// Degenerated in at least 2 vertical lines
			// The algorithm below doesn't work when face has a single pixel width
			// We cannot use general formulas because the plane is degenerated.
			// We have to interpolate along the 3 edges that overlaps and process each pixel.
			// sort the y coord to make formula simpler
			int ytmp;
			btScalar ztmp;
			if (y[0] > y[1]) {
				ytmp = y[1];
				y[1] = y[0];
				y[0] = ytmp;
				ztmp = z[1];
				z[1] = z[0];
				z[0] = ztmp;
			}
			if (y[0] > y[2]) {
				ytmp = y[2];
				y[2] = y[0];
				y[0] = ytmp;
				ztmp = z[2];
				z[2] = z[0];
				z[0] = ztmp;
			}
			if (y[1] > y[2]) {
				ytmp = y[2];
				y[2] = y[1];
				y[1] = ytmp;
				ztmp = z[2];
				z[2] = z[1];
				z[1] = ztmp;
			}
			int dy[] = {y[0] - y[1],
				        y[1] - y[2],
				        y[2] - y[0]};
			btScalar dzy[3];
			dzy[0] = (dy[0]) ? (z[0] - z[1]) / dy[0] : btScalar(0.0f);
			dzy[1] = (dy[1]) ? (z[1] - z[2]) / dy[1] : btScalar(0.0f);
			dzy[2] = (dy[2]) ? (z[2] - z[0]) / dy[2] : btScalar(0.0f);
			btScalar v[3] = {dzy[0] * (miy - y[0]) + z[0],
				             dzy[1] * (miy - y[1]) + z[1],
				             dzy[2] * (miy - y[2]) + z[2]};
			dy[0] = y[1] - y[0];
			dy[1] = y[0] - y[1];
			dy[2] = y[2] - y[0];
			btScalar *scan = &m_buffer[miy * m_sizes[0] + mix];
			for (int iy = miy; iy < mxy; ++iy) {
				if (dy[0] >= 0 && POLICY::Process(*scan, v[0])) {
					return true;
				}
				if (dy[1] >= 0 && POLICY::Process(*scan, v[1])) {
					return true;
				}
				if (dy[2] >= 0 && POLICY::Process(*scan, v[2])) {
					return true;
				}
				scan += m_sizes[0];
				v[0] += dzy[0];
				v[1] += dzy[1];
				v[2] += dzy[2];
				dy[0]--;
				dy[1]++;
				dy[2]--;
			}
		}
		else if (height == 1) {
			// Degenerated in at least 2 horizontal lines
			// The algorithm below doesn't work when face has a single pixel width
			// We cannot use general formulas because the plane is degenerated.
			// We have to interpolate along the 3 edges that overlaps and process each pixel.
			int xtmp;
			btScalar ztmp;
			if (x[0] > x[1]) {
				xtmp = x[1];
				x[1] = x[0];
				x[0] = xtmp;
				ztmp = z[1];
				z[1] = z[0];
				z[0] = ztmp;
			}
			if (x[0] > x[2]) {
				xtmp = x[2];
				x[2] = x[0];
				x[0] = xtmp;
				ztmp = z[2];
				z[2] = z[0];
				z[0] = ztmp;
			}
			if (x[1] > x[2]) {
				xtmp = x[2];
				x[2] = x[1];
				x[1] = xtmp;
				ztmp = z[2];
				z[2] = z[1];
				z[1] = ztmp;
			}
			int dx[] = {x[0] - x[1],
				        x[1] - x[2],
				        x[2] - x[0]};
			btScalar dzx[3];
			dzx[0] = (dx[0]) ? (z[0] - z[1]) / dx[0] : btScalar(0.0f);
			dzx[1] = (dx[1]) ? (z[1] - z[2]) / dx[1] : btScalar(0.0f);
			dzx[2] = (dx[2]) ? (z[2] - z[0]) / dx[2] : btScalar(0.0f);
			btScalar v[3] = {dzx[0] * (mix - x[0]) + z[0],
				             dzx[1] * (mix - x[1]) + z[1],
				             dzx[2] * (mix - x[2]) + z[2]};
			dx[0] = x[1] - x[0];
			dx[1] = x[0] - x[1];
			dx[2] = x[2] - x[0];
			btScalar *scan = &m_buffer[miy * m_sizes[0] + mix];
			for (int ix = mix; ix < mxx; ++ix) {
				if (dx[0] >= 0 && POLICY::Process(*scan, v[0])) {
					return true;
				}
				if (dx[1] >= 0 && POLICY::Process(*scan, v[1])) {
					return true;
				}
				if (dx[2] >= 0 && POLICY::Process(*scan, v[2])) {
					return true;
				}
				scan++;
				v[0] += dzx[0];
				v[1] += dzx[1];
				v[2] += dzx[2];
				dx[0]--;
				dx[1]++;
				dx[2]--;
			}
		}
		else {
			// general case
			const int dx[] = {y[0] - y[1],
				              y[1] - y[2],
				              y[2] - y[0]};
			const int dy[] = {x[1] - x[0] - dx[0] * width,
				              x[2] - x[1] - dx[1] * width,
				              x[0] - x[2] - dx[2] * width};
			const int a = x[2] * y[0] + x[0] * y[1] - x[2] * y[1] - x[0] * y[2] + x[1] * y[2] - x[1] * y[0];
			const btScalar ia = 1 / (btScalar)a;
			const btScalar dzx = ia * (y[2] * (z[1] - z[0]) + y[1] * (z[0] - z[2]) + y[0] * (z[2] - z[1]));
			const btScalar dzy = ia * (x[2] * (z[0] - z[1]) + x[0] * (z[1] - z[2]) + x[1] * (z[2] - z[0])) - (dzx * width);
			int c[] = {miy *x[1] + mix * y[0] - x[1] * y[0] - mix * y[1] + x[0] * y[1] - miy * x[0],
				miy *x[2] + mix * y[1] - x[2] * y[1] - mix * y[2] + x[1] * y[2] - miy * x[1],
				miy *x[0] + mix * y[2] - x[0] * y[2] - mix * y[0] + x[2] * y[0] - miy * x[2]};
			btScalar v = ia * ((z[2] * c[0]) + (z[0] * c[1]) + (z[1] * c[2]));
			btScalar *scan = &m_buffer[miy * m_sizes[0]];

			for (int iy = miy; iy < mxy; ++iy) {
				for (int ix = mix; ix < mxx; ++ix) {
					if ((c[0] >= 0) && (c[1] >= 0) && (c[2] >= 0)) {
						if (POLICY::Process(scan[ix], v)) {
							return true;
						}
					}
					c[0] += dx[0]; c[1] += dx[1]; c[2] += dx[2]; v += dzx;
				}
				c[0] += dy[0]; c[1] += dy[1]; c[2] += dy[2]; v += dzy;
				scan += m_sizes[0];
			}
		}
		return false;
	}
	// clip than write or check a polygon
	template <const int NP, typename POLICY>
	inline bool clipDraw(const btVector4 *p,
	                     const float face,
	                     btScalar minarea)
	{
		btVector4 o[NP * 2];
		int n = clip<NP>(p, o);
		bool earlyexit = false;
		if (n) {
			project(o, n);
			for (int i = 2; i < n && !earlyexit; ++i) {
				earlyexit |= draw<POLICY>(o[0], o[i - 1], o[i], face, minarea);
			}
		}
		return earlyexit;
	}
	// add a triangle (in model coordinate)
	// face =  0.f if face is double side,
	//      =  1.f if face is single sided and scale is positive
	//      = -1.f if face is single sided and scale is negative
	void appendOccluderM(const float *a,
	                     const float *b,
	                     const float *c,
	                     const float face)
	{
		btVector4 p[3];
		transformM(a, p[0]);
		transformM(b, p[1]);
		transformM(c, p[2]);
		clipDraw<3, WriteOCL>(p, face, btScalar(0.0f));
	}

	// query occluder for a box (c=center, e=extend) in world coordinate
	inline bool queryOccluderW(const btVector3 &c,
	                           const btVector3 &e)
	{
		if (!m_occlusion) {
			// no occlusion yet, no need to check
			return true;
		}
		btVector4 x[8];
		transformW(btVector3(c[0] - e[0], c[1] - e[1], c[2] - e[2]), x[0]);
		transformW(btVector3(c[0] + e[0], c[1] - e[1], c[2] - e[2]), x[1]);
		transformW(btVector3(c[0] + e[0], c[1] + e[1], c[2] - e[2]), x[2]);
		transformW(btVector3(c[0] - e[0], c[1] + e[1], c[2] - e[2]), x[3]);
		transformW(btVector3(c[0] - e[0], c[1] - e[1], c[2] + e[2]), x[4]);
		transformW(btVector3(c[0] + e[0], c[1] - e[1], c[2] + e[2]), x[5]);
		transformW(btVector3(c[0] + e[0], c[1] + e[1], c[2] + e[2]), x[6]);
		transformW(btVector3(c[0] - e[0], c[1] + e[1], c[2] + e[2]), x[7]);

		for (int i = 0; i < 8; ++i) {
			// the box is clipped, it's probably a large box, don't waste our time to check
			if ((x[i][2] + x[i][3]) <= 0) {
				return true;
			}
		}
		static const int d[] = {1, 0, 3, 2,
			                    4, 5, 6, 7,
			                    4, 7, 3, 0,
			                    6, 5, 1, 2,
			                    7, 6, 2, 3,
			                    5, 4, 0, 1};
		for (unsigned int i = 0; i < (sizeof(d) / sizeof(d[0])); ) {
			const btVector4 p[] = {x[d[i + 0]],
				                   x[d[i + 1]],
				                   x[d[i + 2]],
				                   x[d[i + 3]]};
			i += 4;
			if (clipDraw<4, QueryOCL>(p, 1.0f, 0.0f)) {
				return true;
			}
		}
		return false;
	}
};


struct  DbvtCullingCallback : btDbvt::ICollide {
	PHY_CullingCallback m_clientCallback;
	void *m_userData;
	OcclusionBuffer *m_ocb;

	DbvtCullingCallback(PHY_CullingCallback clientCallback, void *userData)
	{
		m_clientCallback = clientCallback;
		m_userData = userData;
		m_ocb = nullptr;
	}
	bool Descent(const btDbvtNode *node)
	{
		return (m_ocb->queryOccluderW(node->volume.Center(), node->volume.Extents()));
	}
	void Process(const btDbvtNode *node, btScalar depth)
	{
		Process(node);
	}
	void Process(const btDbvtNode *leaf)
	{
		btBroadphaseProxy *proxy = (btBroadphaseProxy *)leaf->data;
		// the client object is a graphic controller
		CcdGraphicController *ctrl = static_cast<CcdGraphicController *>(proxy->m_clientObject);
		KX_ClientObjectInfo *info = (KX_ClientObjectInfo *)ctrl->GetNewClientInfo();
		if (m_ocb) {
			// means we are doing occlusion culling. Check if this object is an occluders
			KX_GameObject *gameobj = KX_GameObject::GetClientObject(info);
			if (gameobj && gameobj->GetOccluder()) {
				float fl[16];
				gameobj->NodeGetWorldTransform().PackFromAffineTransform(fl);

				// this will create the occlusion buffer if not already done
				// and compute the transformation from model local space to clip space
				m_ocb->SetModelMatrix(fl);
				const float negative = gameobj->IsNegativeScaling();
				// walk through the meshes and for each add to buffer
				for (KX_Mesh *meshobj : gameobj->GetMeshList()) {
					for (RAS_MeshMaterial *meshmat : meshobj->GetMeshMaterialList()) {
						RAS_DisplayArray *array = meshmat->GetDisplayArray();
						const bool twoside = meshmat->GetBucket()->GetMaterial()->IsTwoSided();
						const float face = (twoside) ? 0.0f : ((negative) ? -1.0f : 1.0f);

						for (unsigned int j = 0, size = array->GetTriangleIndexCount(); j < size; j += 3) {
							m_ocb->appendOccluderM(array->GetPosition(array->GetTriangleIndex(j)).data,
							                       array->GetPosition(array->GetTriangleIndex(j + 1)).data,
							                       array->GetPosition(array->GetTriangleIndex(j + 2)).data,
							                       face);
						}
					}
				}
			}
		}
		if (info) {
			(*m_clientCallback)(info, m_userData);
		}
	}
};

static OcclusionBuffer gOcb;
bool CcdPhysicsEnvironment::CullingTest(PHY_CullingCallback callback, void *userData, const std::array<mt::vec4, 6>& planes,
                                        int occlusionRes, const int *viewport, const mt::mat4& matrix)
{
	if (!m_cullingTree) {
		return false;
	}
	DbvtCullingCallback dispatcher(callback, userData);
	btVector3 planes_n[6];
	btScalar planes_o[6];
	for (int i = 0; i < 6; i++) {
		planes_n[i] = ToBullet(planes[i]);
		planes_o[i] = planes[i][3];
	}
	// if occlusionRes != 0 => occlusion culling
	if (occlusionRes) {
		gOcb.setup(occlusionRes, viewport, (float *)matrix.Data());
		dispatcher.m_ocb = &gOcb;
		// occlusion culling, the direction of the view is taken from the first plan which MUST be the near plane
		btDbvt::collideOCL(m_cullingTree->m_sets[1].m_root, planes_n, planes_o, planes_n[0], 6, dispatcher);
		btDbvt::collideOCL(m_cullingTree->m_sets[0].m_root, planes_n, planes_o, planes_n[0], 6, dispatcher);
	}
	else {
		btDbvt::collideKDOP(m_cullingTree->m_sets[1].m_root, planes_n, planes_o, 6, dispatcher);
		btDbvt::collideKDOP(m_cullingTree->m_sets[0].m_root, planes_n, planes_o, 6, dispatcher);
	}
	return true;
}

int CcdPhysicsEnvironment::GetNumContactPoints()
{
	return 0;
}

void CcdPhysicsEnvironment::GetContactPoint(int i, float& hitX, float& hitY, float& hitZ, float& normalX, float& normalY, float& normalZ)
{
}

btBroadphaseInterface *CcdPhysicsEnvironment::GetBroadphase()
{
	return m_dynamicsWorld->getBroadphase();
}

btDispatcher *CcdPhysicsEnvironment::GetDispatcher()
{
	return m_dynamicsWorld->getDispatcher();
}

void CcdPhysicsEnvironment::MergeEnvironment(PHY_IPhysicsEnvironment *other_env)
{
	CcdPhysicsEnvironment *other = static_cast<CcdPhysicsEnvironment *>(other_env);
	if (other == nullptr) {
		CM_Error("other scene is not using Bullet physics, not merging physics.");
		return;
	}

	std::set<CcdPhysicsController *>::iterator it;

	while (other->m_controllers.begin() != other->m_controllers.end()) {
		it = other->m_controllers.begin();
		CcdPhysicsController *ctrl = (*it);

		other->RemoveCcdPhysicsController(ctrl, true);
		this->AddCcdPhysicsController(ctrl);
	}
}

CcdPhysicsEnvironment::~CcdPhysicsEnvironment()
{
	m_wrapperVehicles.clear();

	// First delete scene, then dispatcher, because pairs have to release manifolds on the dispatcher.
	m_dynamicsWorld.reset(nullptr);
}

btTypedConstraint *CcdPhysicsEnvironment::GetConstraintById(int constraintId)
{
	// For soft body constraints
	if (constraintId == 0) {
		return nullptr;
	}

	int numConstraints = m_dynamicsWorld->getNumConstraints();
	int i;
	for (i = 0; i < numConstraints; i++) {
		btTypedConstraint *constraint = m_dynamicsWorld->getConstraint(i);
		if (constraint->getUserConstraintId() == constraintId) {
			return constraint;
		}
	}
	return nullptr;
}

void CcdPhysicsEnvironment::AddSensor(PHY_IPhysicsController *ctrl)
{
	CcdPhysicsController *ctrl1 = (CcdPhysicsController *)ctrl;
	AddCcdPhysicsController(ctrl1);
}

bool CcdPhysicsEnvironment::RemoveCollisionCallback(PHY_IPhysicsController *ctrl)
{
	CcdPhysicsController *ccdCtrl = (CcdPhysicsController *)ctrl;
	return ccdCtrl->Unregister();
}

void CcdPhysicsEnvironment::RemoveSensor(PHY_IPhysicsController *ctrl)
{
	RemoveCcdPhysicsController((CcdPhysicsController *)ctrl, true);
}

void CcdPhysicsEnvironment::AddCollisionCallback(int response_class, PHY_ResponseCallback callback, void *user)
{
	m_triggerCallbacks[response_class] = callback;
	m_triggerCallbacksUserPtrs[response_class] = user;
}
bool CcdPhysicsEnvironment::RequestCollisionCallback(PHY_IPhysicsController *ctrl)
{
	CcdPhysicsController *ccdCtrl = static_cast<CcdPhysicsController *>(ctrl);
	return ccdCtrl->Register();
}

void CcdPhysicsEnvironment::CallbackTriggers()
{
	if (!m_triggerCallbacks[PHY_OBJECT_RESPONSE]) {
		return;
	}

	// Walk over all overlapping pairs, and if one of the involved bodies is registered for trigger callback, perform callback.
	btDispatcher *dispatcher = m_dynamicsWorld->getDispatcher();
	for (unsigned int i = 0, numManifolds = dispatcher->getNumManifolds(); i < numManifolds; i++) {
		btPersistentManifold *manifold = dispatcher->getManifoldByIndexInternal(i);
		if (manifold->getNumContacts() == 0) {
			continue;
		}

		const btCollisionObject *col0 = manifold->getBody0();
		const btCollisionObject *col1 = manifold->getBody1();

		CcdPhysicsController *ctrl0 = static_cast<CcdPhysicsController *>(col0->getUserPointer());
		CcdPhysicsController *ctrl1 = static_cast<CcdPhysicsController *>(col1->getUserPointer());

		bool first;
		// Test if one of the controller is registered and use collision callback.
		if (ctrl0->Registered()) {
			first = true;
		}
		else if (ctrl1->Registered()) {
			first = false;
		}
		else {
			// No controllers registered for collision callbacks.
			continue;
		}

		const CcdCollData *coll_data = new CcdCollData(manifold);
		m_triggerCallbacks[PHY_OBJECT_RESPONSE](m_triggerCallbacksUserPtrs[PHY_OBJECT_RESPONSE], ctrl0, ctrl1, coll_data, first);
	}
}

PHY_CollisionTestResult CcdPhysicsEnvironment::CheckCollision(PHY_IPhysicsController *ctrl0, PHY_IPhysicsController *ctrl1)
{
	PHY_CollisionTestResult result{false, false, nullptr};

	btCollisionObject *col0 = static_cast<CcdPhysicsController *>(ctrl0)->GetCollisionObject();
	btCollisionObject *col1 = static_cast<CcdPhysicsController *>(ctrl1)->GetCollisionObject();

	if (!col0 || !col1) {
		return result;
	}

	btBroadphaseProxy *proxy0 = col0->getBroadphaseHandle();
	btBroadphaseProxy *proxy1 = col1->getBroadphaseHandle();

	btBroadphasePair *pair = m_dynamicsWorld->getPairCache()->findPair(proxy0, proxy1);

	if (!pair) {
		return result;
	}

	result.collide = true;

	if (pair->m_algorithm) {
		btManifoldArray manifoldArray;
		pair->m_algorithm->getAllContactManifolds(manifoldArray);
		btPersistentManifold *manifold = manifoldArray[0];

		result.isFirst = (col0 == manifold->getBody0());
		result.collData = new CcdCollData(manifold);
	}

	return result;
}

// This call back is called before a pair is added in the cache
// Handy to remove objects that must be ignored by sensors
bool CcdOverlapFilterCallBack::needBroadphaseCollision(btBroadphaseProxy *proxy0, btBroadphaseProxy *proxy1) const
{
	btCollisionObject *colObj0 = (btCollisionObject *)proxy0->m_clientObject;
	btCollisionObject *colObj1 = (btCollisionObject *)proxy1->m_clientObject;

	if (!colObj0 || !colObj1) {
		return false;
	}

	CcdPhysicsController *ctrl0 = static_cast<CcdPhysicsController *>(colObj0->getUserPointer());
	CcdPhysicsController *ctrl1 = static_cast<CcdPhysicsController *>(colObj1->getUserPointer());

	if (!((proxy0->m_collisionFilterGroup & proxy1->m_collisionFilterMask) &&
	      (proxy1->m_collisionFilterGroup & proxy0->m_collisionFilterMask) &&
	      (ctrl0->GetCollisionGroup() & ctrl1->GetCollisionMask()) &&
	      (ctrl1->GetCollisionGroup() & ctrl0->GetCollisionMask()))) {
		return false;
	}

	CcdPhysicsController *sensorCtrl, *objCtrl;
	// additional check for sensor object
	if (proxy0->m_collisionFilterGroup & btBroadphaseProxy::SensorTrigger) {
		// this is a sensor object, the other one can't be a sensor object because
		// they exclude each other in the above test
		BLI_assert(!(proxy1->m_collisionFilterGroup & btBroadphaseProxy::SensorTrigger));
		sensorCtrl = ctrl0;
		objCtrl = ctrl1;
	}
	else if (proxy1->m_collisionFilterGroup & btBroadphaseProxy::SensorTrigger) {
		sensorCtrl = ctrl1;
		objCtrl = ctrl0;
	}
	else {
		return true;
	}

	if (m_physEnv->m_triggerCallbacks[PHY_BROADPH_RESPONSE]) {
		return m_physEnv->m_triggerCallbacks[PHY_BROADPH_RESPONSE](m_physEnv->m_triggerCallbacksUserPtrs[PHY_BROADPH_RESPONSE], sensorCtrl, objCtrl, nullptr, false);
	}
	return true;
}

//complex constraint for vehicles
PHY_IVehicle *CcdPhysicsEnvironment::GetVehicleConstraint(int constraintId)
{
	int i;

	int numVehicles = m_wrapperVehicles.size();
	for (i = 0; i < numVehicles; i++) {
		WrapperVehicle *wrapperVehicle = m_wrapperVehicles[i];
		if (wrapperVehicle->GetVehicle()->getUserConstraintId() == constraintId) {
			return wrapperVehicle;
		}
	}

	return nullptr;
}

PHY_ICharacter *CcdPhysicsEnvironment::GetCharacterController(KX_GameObject *ob)
{
	CcdPhysicsController *controller = (CcdPhysicsController *)ob->GetPhysicsController();
	return (controller) ? static_cast<CcdCharacter *>(controller->GetCharacterController()) : nullptr;
}


PHY_IPhysicsController *CcdPhysicsEnvironment::CreateSphereController(float radius, const mt::vec3& position)
{
	CcdConstructionInfo cinfo;
	cinfo.m_collisionShape = new btSphereShape(radius); // memory leak! The shape is not deleted by Bullet and we cannot add it to the KX_Scene.m_shapes list
	cinfo.m_MotionState = nullptr;
	cinfo.m_physicsEnv = this;
	// declare this object as Dyamic rather than static!!
	// The reason as it is designed to detect all type of object, including static object
	// It would cause static-static message to be printed on the console otherwise
	cinfo.m_collisionFlags |= btCollisionObject::CF_NO_CONTACT_RESPONSE | btCollisionObject::CF_STATIC_OBJECT;
	DefaultMotionState *motionState = new DefaultMotionState();
	cinfo.m_MotionState = motionState;
	// we will add later the possibility to select the filter from option
	cinfo.m_collisionFilterMask = CcdConstructionInfo::AllFilter ^ CcdConstructionInfo::SensorFilter;
	cinfo.m_collisionFilterGroup = CcdConstructionInfo::SensorFilter;
	cinfo.m_bSensor = true;
	motionState->m_worldTransform.setIdentity();
	motionState->m_worldTransform.setOrigin(ToBullet(position));

	CcdPhysicsController *sphereController = new CcdPhysicsController(cinfo);

	return sphereController;
}

int Ccd_FindClosestNode(btSoftBody *sb, const btVector3& worldPoint)
{
	int node = -1;

	btSoftBody::tNodeArray&   nodes(sb->m_nodes);
	float maxDistSqr = 1e30f;

	for (int n = 0; n < nodes.size(); n++) {
		btScalar distSqr = (nodes[n].m_x - worldPoint).length2();
		if (distSqr < maxDistSqr) {
			maxDistSqr = distSqr;
			node = n;
		}
	}
	return node;
}

PHY_IConstraint *CcdPhysicsEnvironment::CreateConstraint(class PHY_IPhysicsController *ctrl0, class PHY_IPhysicsController *ctrl1, PHY_ConstraintType type,
																 float pivotX, float pivotY, float pivotZ,
																 float axisX, float axisY, float axisZ,
																 float axis1X, float axis1Y, float axis1Z,
																 float axis2X, float axis2Y, float axis2Z, int flags)
{
	bool disableCollisionBetweenLinkedBodies = (0 != (flags & CCD_CONSTRAINT_DISABLE_LINKED_COLLISION));

	CcdPhysicsController *c0 = (CcdPhysicsController *)ctrl0;
	CcdPhysicsController *c1 = (CcdPhysicsController *)ctrl1;

	btRigidBody *rb0 = c0 ? c0->GetRigidBody() : nullptr;
	btRigidBody *rb1 = c1 ? c1->GetRigidBody() : nullptr;

	bool rb0static = rb0 ? rb0->isStaticOrKinematicObject() : true;
	bool rb1static = rb1 ? rb1->isStaticOrKinematicObject() : true;

	btCollisionObject *colObj0 = c0->GetCollisionObject();
	if (!colObj0) {
		return nullptr;
	}

	btVector3 pivotInA(pivotX, pivotY, pivotZ);

	//it might be a soft body, let's try
	btSoftBody *sb0 = c0 ? c0->GetSoftBody() : nullptr;
	btSoftBody *sb1 = c1 ? c1->GetSoftBody() : nullptr;
	if (sb0 && sb1) {
		//not between two soft bodies?
		return nullptr;
	}

	if (sb0) {
		//either cluster or node attach, let's find closest node first
		//the soft body doesn't have a 'real' world transform, so get its initial world transform for now
		btVector3 pivotPointSoftWorld = sb0->m_initialWorldTransform(pivotInA);
		int node = Ccd_FindClosestNode(sb0, pivotPointSoftWorld);
		if (node >= 0) {
			if (rb1) {
				sb0->appendAnchor(node, rb1, disableCollisionBetweenLinkedBodies);
			}
			else {
				sb0->setMass(node, 0.0f);
			}
		}
		return nullptr;//can't remove soft body anchors yet
	}

	if (sb1) {
		btVector3 pivotPointAWorld = colObj0->getWorldTransform()(pivotInA);
		int node = Ccd_FindClosestNode(sb1, pivotPointAWorld);
		if (node >= 0) {
			if (rb0) {
				sb1->appendAnchor(node, rb0, disableCollisionBetweenLinkedBodies);
			}
			else {
				sb1->setMass(node, 0.0f);
			}
		}
		return nullptr;//can't remove soft body anchors yet
	}

	if (rb0static && rb1static) {
		return nullptr;
	}


	if (!rb0) {
		return nullptr;
	}

	// If either of the controllers is missing, we can't do anything.
	if (!c0 || !c1) {
		return nullptr;
	}

	btTypedConstraint *con = nullptr;

	btVector3 pivotInB = rb1 ? rb1->getCenterOfMassTransform().inverse()(rb0->getCenterOfMassTransform()(pivotInA)) :
	                     rb0->getCenterOfMassTransform() * pivotInA;
	btVector3 axisInA(axisX, axisY, axisZ);


	bool angularOnly = false;

	switch (type) {
		case PHY_POINT2POINT_CONSTRAINT:
		{
			btPoint2PointConstraint *p2p = nullptr;

			if (rb1) {
				p2p = new btPoint2PointConstraint(*rb0, *rb1, pivotInA, pivotInB);
			}
			else {
				p2p = new btPoint2PointConstraint(*rb0, pivotInA);
			}

			con = p2p;

			break;
		}

		case PHY_GENERIC_6DOF_CONSTRAINT:
		{
			btGeneric6DofConstraint *genericConstraint = nullptr;

			if (rb1) {
				btTransform frameInA;
				btTransform frameInB;

				btVector3 axis1(axis1X, axis1Y, axis1Z), axis2(axis2X, axis2Y, axis2Z);
				if (axis1.length() == 0.0) {
					btPlaneSpace1(axisInA, axis1, axis2);
				}

				frameInA.getBasis().setValue(axisInA.x(), axis1.x(), axis2.x(),
				                             axisInA.y(), axis1.y(), axis2.y(),
				                             axisInA.z(), axis1.z(), axis2.z());
				frameInA.setOrigin(pivotInA);

				btTransform inv = rb1->getCenterOfMassTransform().inverse();

				btTransform globalFrameA = rb0->getCenterOfMassTransform() * frameInA;

				frameInB = inv * globalFrameA;
				bool useReferenceFrameA = true;

				genericConstraint = new btGeneric6DofSpringConstraint(
					*rb0, *rb1,
					frameInA, frameInB, useReferenceFrameA);
			}
			else {
				static btRigidBody s_fixedObject2(0.0f, nullptr, nullptr);
				btTransform frameInA;
				btTransform frameInB;

				btVector3 axis1, axis2;
				btPlaneSpace1(axisInA, axis1, axis2);

				frameInA.getBasis().setValue(axisInA.x(), axis1.x(), axis2.x(),
				                             axisInA.y(), axis1.y(), axis2.y(),
				                             axisInA.z(), axis1.z(), axis2.z());

				frameInA.setOrigin(pivotInA);

				///frameInB in worldspace
				frameInB = rb0->getCenterOfMassTransform() * frameInA;

				bool useReferenceFrameA = true;
				genericConstraint = new btGeneric6DofSpringConstraint(
					*rb0, s_fixedObject2,
					frameInA, frameInB, useReferenceFrameA);
			}

			con = genericConstraint;

			break;
		}
		case PHY_CONE_TWIST_CONSTRAINT:
		{
			btConeTwistConstraint *coneTwistContraint = nullptr;

			if (rb1) {
				btTransform frameInA;
				btTransform frameInB;

				btVector3 axis1(axis1X, axis1Y, axis1Z), axis2(axis2X, axis2Y, axis2Z);
				if (axis1.length() == 0.0) {
					btPlaneSpace1(axisInA, axis1, axis2);
				}

				frameInA.getBasis().setValue(axisInA.x(), axis1.x(), axis2.x(),
				                             axisInA.y(), axis1.y(), axis2.y(),
				                             axisInA.z(), axis1.z(), axis2.z());
				frameInA.setOrigin(pivotInA);

				btTransform inv = rb1->getCenterOfMassTransform().inverse();

				btTransform globalFrameA = rb0->getCenterOfMassTransform() * frameInA;

				frameInB = inv * globalFrameA;

				coneTwistContraint = new btConeTwistConstraint(*rb0, *rb1,
				                                               frameInA, frameInB);
			}
			else {
				static btRigidBody s_fixedObject2(0.0f, nullptr, nullptr);
				btTransform frameInA;
				btTransform frameInB;

				btVector3 axis1, axis2;
				btPlaneSpace1(axisInA, axis1, axis2);

				frameInA.getBasis().setValue(axisInA.x(), axis1.x(), axis2.x(),
				                             axisInA.y(), axis1.y(), axis2.y(),
				                             axisInA.z(), axis1.z(), axis2.z());

				frameInA.setOrigin(pivotInA);

				///frameInB in worldspace
				frameInB = rb0->getCenterOfMassTransform() * frameInA;

				coneTwistContraint = new btConeTwistConstraint(
					*rb0, s_fixedObject2,
					frameInA, frameInB);
			}

			con = coneTwistContraint;

			break;
		}
		case PHY_ANGULAR_CONSTRAINT:
		{
			angularOnly = true;

		}
		case PHY_LINEHINGE_CONSTRAINT:
		{
			btHingeConstraint *hinge = nullptr;

			if (rb1) {
				// We know the orientations so we should use them instead of
				// having btHingeConstraint fill in the blanks any way it wants to.
				btTransform frameInA;
				btTransform frameInB;

				btVector3 axis1(axis1X, axis1Y, axis1Z), axis2(axis2X, axis2Y, axis2Z);
				if (axis1.length() == 0.0f) {
					btPlaneSpace1(axisInA, axis1, axis2);
				}

				// Internally btHingeConstraint's hinge-axis is z
				frameInA.getBasis().setValue(axis1.x(), axis2.x(), axisInA.x(),
				                             axis1.y(), axis2.y(), axisInA.y(),
				                             axis1.z(), axis2.z(), axisInA.z());

				frameInA.setOrigin(pivotInA);

				btTransform inv = rb1->getCenterOfMassTransform().inverse();

				btTransform globalFrameA = rb0->getCenterOfMassTransform() * frameInA;

				frameInB = inv  * globalFrameA;

				hinge = new btHingeConstraint(*rb0, *rb1, frameInA, frameInB);
			}
			else {
				static btRigidBody s_fixedObject2(0.0f, nullptr, nullptr);

				btTransform frameInA;
				btTransform frameInB;

				btVector3 axis1(axis1X, axis1Y, axis1Z), axis2(axis2X, axis2Y, axis2Z);
				if (axis1.length() == 0.0f) {
					btPlaneSpace1(axisInA, axis1, axis2);
				}

				// Internally btHingeConstraint's hinge-axis is z
				frameInA.getBasis().setValue(axis1.x(), axis2.x(), axisInA.x(),
				                             axis1.y(), axis2.y(), axisInA.y(),
				                             axis1.z(), axis2.z(), axisInA.z());
				frameInA.setOrigin(pivotInA);
				frameInB = rb0->getCenterOfMassTransform() * frameInA;

				hinge = new btHingeConstraint(*rb0, s_fixedObject2, frameInA, frameInB);
			}
			hinge->setAngularOnly(angularOnly);

			con = hinge;

			break;
		}
		default:
		{
		}
	}
	;

	if (!con) {
		return nullptr;
	}

	c0->addCcdConstraintRef(con);
	c1->addCcdConstraintRef(con);
	con->setUserConstraintId(gConstraintUid++);
	con->setUserConstraintType(type);
	CcdConstraint *constraintData = new CcdConstraint(con, disableCollisionBetweenLinkedBodies);
	con->setUserConstraintPtr(constraintData);
	m_dynamicsWorld->addConstraint(con, disableCollisionBetweenLinkedBodies);

	return constraintData;
}

PHY_IVehicle *CcdPhysicsEnvironment::CreateVehicle(PHY_IPhysicsController *ctrl)
{
	const btRaycastVehicle::btVehicleTuning tuning = btRaycastVehicle::btVehicleTuning();
	BlenderVehicleRaycaster *raycaster = new BlenderVehicleRaycaster(m_dynamicsWorld.get());
	btRaycastVehicle *vehicle = new btRaycastVehicle(tuning, ((CcdPhysicsController *)ctrl)->GetRigidBody(), raycaster);
	WrapperVehicle *wrapperVehicle = new WrapperVehicle(vehicle, raycaster, ctrl);
	m_wrapperVehicles.push_back(wrapperVehicle);

	m_dynamicsWorld->addVehicle(vehicle);

	vehicle->setUserConstraintId(gConstraintUid++);
	vehicle->setUserConstraintType(PHY_VEHICLE_CONSTRAINT);

	return wrapperVehicle;
}

PHY_IPhysicsController *CcdPhysicsEnvironment::CreateConeController(float coneradius, float coneheight)
{
	CcdConstructionInfo cinfo;

	// we don't need a CcdShapeConstructionInfo for this shape:
	// it is simple enough for the standard copy constructor (see CcdPhysicsController::GetReplica)
	cinfo.m_collisionShape = new btConeShape(coneradius, coneheight);
	cinfo.m_MotionState = nullptr;
	cinfo.m_physicsEnv = this;
	cinfo.m_collisionFlags |= btCollisionObject::CF_NO_CONTACT_RESPONSE | btCollisionObject::CF_STATIC_OBJECT;
	DefaultMotionState *motionState = new DefaultMotionState();
	cinfo.m_MotionState = motionState;

	// we will add later the possibility to select the filter from option
	cinfo.m_collisionFilterMask = CcdConstructionInfo::AllFilter ^ CcdConstructionInfo::SensorFilter;
	cinfo.m_collisionFilterGroup = CcdConstructionInfo::SensorFilter;
	cinfo.m_bSensor = true;
	motionState->m_worldTransform.setIdentity();
//	motionState->m_worldTransform.setOrigin(btVector3(position[0],position[1],position[2]));

	CcdPhysicsController *sphereController = new CcdPhysicsController(cinfo);

	return sphereController;
}

float CcdPhysicsEnvironment::getAppliedImpulse(int constraintid)
{
	// For soft body constraints
	if (constraintid == 0) {
		return 0.0f;
	}

	int i;
	int numConstraints = m_dynamicsWorld->getNumConstraints();
	for (i = 0; i < numConstraints; i++) {
		btTypedConstraint *constraint = m_dynamicsWorld->getConstraint(i);
		if (constraint->getUserConstraintId() == constraintid) {
			return constraint->getAppliedImpulse();
		}
	}

	return 0.0f;
}

void CcdPhysicsEnvironment::ExportFile(const std::string& filename)
{
	btDefaultSerializer *serializer = new btDefaultSerializer();

	for (int i = 0; i < m_dynamicsWorld->getNumCollisionObjects(); i++) {
		btCollisionObject *colObj = m_dynamicsWorld->getCollisionObjectArray()[i];

		CcdPhysicsController *controller = static_cast<CcdPhysicsController *>(colObj->getUserPointer());
		if (controller) {
			const std::string name = KX_GameObject::GetClientObject((KX_ClientObjectInfo *)controller->GetNewClientInfo())->GetName();
			if (!name.empty()) {
				serializer->registerNameForPointer(colObj, name.c_str());
			}
		}
	}

	m_dynamicsWorld->serialize(serializer);

	FILE *file = fopen(filename.c_str(), "wb");
	if (file) {
		fwrite(serializer->getBufferPointer(), serializer->getCurrentBufferSize(), 1, file);
		fclose(file);
	}
}

CcdPhysicsEnvironment *CcdPhysicsEnvironment::Create(Scene *blenderscene, bool visualizePhysics)
{
	static const PHY_SolverType solverTypeTable[] = {
		PHY_SOLVER_SEQUENTIAL, // GAME_SOLVER_SEQUENTIAL
		PHY_SOLVER_NNCG, // GAME_SOLVER_NNGC
		PHY_SOLVER_MLCP_DANTZIG, // GAME_SOLVER_MLCP_DANTZIG
		PHY_SOLVER_MLCP_LEMKE // GAME_SOLVER_MLCP_LEMKE
	};

	CcdPhysicsEnvironment *ccdPhysEnv = new CcdPhysicsEnvironment(solverTypeTable[blenderscene->gm.solverType],
	                                                              (blenderscene->gm.mode & WO_DBVT_CULLING) != 0);

	ccdPhysEnv->SetDeactivationLinearTreshold(blenderscene->gm.lineardeactthreshold);
	ccdPhysEnv->SetDeactivationAngularTreshold(blenderscene->gm.angulardeactthreshold);
	ccdPhysEnv->SetDeactivationTime(blenderscene->gm.deactivationtime);

	if (visualizePhysics) {
		ccdPhysEnv->SetDebugMode(btIDebugDraw::DBG_DrawWireframe | btIDebugDraw::DBG_DrawAabb | btIDebugDraw::DBG_DrawContactPoints |
		                         btIDebugDraw::DBG_DrawText | btIDebugDraw::DBG_DrawConstraintLimits | btIDebugDraw::DBG_DrawConstraints);
	}

	return ccdPhysEnv;
}

void CcdPhysicsEnvironment::ConvertObject(BL_SceneConverter& converter, KX_GameObject *gameobj, RAS_Mesh *meshobj,
                                          KX_Scene *kxscene, PHY_IMotionState *motionstate,
                                          int activeLayerBitInfo, bool isCompoundChild, bool hasCompoundChildren)
{
	Object *blenderobject = gameobj->GetBlenderObject();

	bool isbulletdyna = (blenderobject->gameflag & OB_DYNAMIC) != 0;
	bool isbulletsensor = (blenderobject->gameflag & OB_SENSOR) != 0;
	bool isbulletchar = (blenderobject->gameflag & OB_CHARACTER) != 0;
	bool isbulletsoftbody = (blenderobject->gameflag & OB_SOFT_BODY) != 0;
	bool isbulletrigidbody = (blenderobject->gameflag & OB_RIGID_BODY) != 0;
	bool useGimpact = false;
	CcdConstructionInfo ci;
	class CcdShapeConstructionInfo *shapeInfo = new CcdShapeConstructionInfo();

	Object *blenderRoot = blenderobject->parent;
	Object *blenderCompoundRoot = nullptr;
	// Iterate over all parents in the object tree.
	{
		Object *parentit = blenderobject->parent;
		while (parentit) {
			// If the parent is valid for compound parent shape, update blenderCompoundRoot.
			if ((parentit->gameflag & OB_CHILD) && (blenderobject->gameflag & (OB_COLLISION | OB_DYNAMIC | OB_RIGID_BODY)) &&
			    !(blenderobject->gameflag & OB_SOFT_BODY)) {
				blenderCompoundRoot = parentit;
			}
			// Continue looking for root parent.
			blenderRoot = parentit;

			parentit = parentit->parent;
		}
	}

	KX_GameObject *compoundParent = nullptr;
	if (blenderCompoundRoot) {
		compoundParent = converter.FindGameObject(blenderCompoundRoot);
		isbulletsoftbody = false;
	}

	KX_GameObject *parentRoot = nullptr;
	if (blenderRoot) {
		parentRoot = converter.FindGameObject(blenderRoot);
		isbulletsoftbody = false;
	}

	if (!isbulletdyna) {
		ci.m_collisionFlags |= btCollisionObject::CF_STATIC_OBJECT;
	}
	if ((blenderobject->gameflag & (OB_GHOST | OB_SENSOR | OB_CHARACTER)) != 0) {
		ci.m_collisionFlags |= btCollisionObject::CF_NO_CONTACT_RESPONSE;
	}

	ci.m_collisionGroup = blenderobject->col_group;
	ci.m_collisionMask = blenderobject->col_mask;

	ci.m_MotionState = motionstate;
	ci.m_gravity = btVector3(0.0f, 0.0f, 0.0f);
	ci.m_linearFactor = btVector3(((blenderobject->gameflag2 & OB_LOCK_RIGID_BODY_X_AXIS) != 0) ? 0.0f : 1.0f,
	                              ((blenderobject->gameflag2 & OB_LOCK_RIGID_BODY_Y_AXIS) != 0) ? 0.0f : 1.0f,
	                              ((blenderobject->gameflag2 & OB_LOCK_RIGID_BODY_Z_AXIS) != 0) ? 0.0f : 1.0f);
	ci.m_angularFactor = btVector3(((blenderobject->gameflag2 & OB_LOCK_RIGID_BODY_X_ROT_AXIS) != 0) ? 0.0f : 1.0f,
	                               ((blenderobject->gameflag2 & OB_LOCK_RIGID_BODY_Y_ROT_AXIS) != 0) ? 0.0f : 1.0f,
	                               ((blenderobject->gameflag2 & OB_LOCK_RIGID_BODY_Z_ROT_AXIS) != 0) ? 0.0f : 1.0f);
	ci.m_localInertiaTensor = btVector3(0.0f, 0.0f, 0.0f);
	ci.m_mass = isbulletdyna ? blenderobject->mass : 0.0f;
	ci.m_clamp_vel_min = blenderobject->min_vel;
	ci.m_clamp_vel_max = blenderobject->max_vel;
	ci.m_clamp_angvel_min = blenderobject->min_angvel;
	ci.m_clamp_angvel_max = blenderobject->max_angvel;
	ci.m_stepHeight = isbulletchar ? blenderobject->step_height : 0.0f;
	ci.m_jumpSpeed = isbulletchar ? blenderobject->jump_speed : 0.0f;
	ci.m_fallSpeed = isbulletchar ? blenderobject->fall_speed : 0.0f;
	ci.m_maxSlope = isbulletchar ? blenderobject->max_slope : 0.0f;
	ci.m_maxJumps = isbulletchar ? blenderobject->max_jumps : 0;

	//mmm, for now, take this for the size of the dynamicobject
	// Blender uses inertia for radius of dynamic object
	shapeInfo->m_radius = ci.m_radius = blenderobject->inertia;
	useGimpact = ((isbulletdyna || isbulletsensor) && !isbulletsoftbody);

	if (isbulletsoftbody) {
		if (blenderobject->bsoft) {
			ci.m_margin = blenderobject->bsoft->margin;
			ci.m_gamesoftFlag = blenderobject->bsoft->flag;

			ci.m_softBendingDistance = blenderobject->bsoft->bending_dist;

			ci.m_soft_linStiff = blenderobject->bsoft->linStiff;
			ci.m_soft_angStiff = blenderobject->bsoft->angStiff; // angular stiffness 0..1
			ci.m_soft_volume = blenderobject->bsoft->volume; // volume preservation 0..1

			ci.m_soft_viterations = blenderobject->bsoft->viterations; // Velocities solver iterations
			ci.m_soft_piterations = blenderobject->bsoft->piterations; // Positions solver iterations
			ci.m_soft_diterations = blenderobject->bsoft->diterations; // Drift solver iterations
			ci.m_soft_citerations = blenderobject->bsoft->citerations; // Cluster solver iterations

			ci.m_soft_kSRHR_CL = blenderobject->bsoft->kSRHR_CL; // Soft vs rigid hardness [0,1] (cluster only)
			ci.m_soft_kSKHR_CL = blenderobject->bsoft->kSKHR_CL; // Soft vs kinetic hardness [0,1] (cluster only)
			ci.m_soft_kSSHR_CL = blenderobject->bsoft->kSSHR_CL; // Soft vs soft hardness [0,1] (cluster only)
			ci.m_soft_kSR_SPLT_CL = blenderobject->bsoft->kSR_SPLT_CL; // Soft vs rigid impulse split [0,1] (cluster only)

			ci.m_soft_kSK_SPLT_CL = blenderobject->bsoft->kSK_SPLT_CL; // Soft vs rigid impulse split [0,1] (cluster only)
			ci.m_soft_kSS_SPLT_CL = blenderobject->bsoft->kSS_SPLT_CL; // Soft vs rigid impulse split [0,1] (cluster only)
			ci.m_soft_kVCF = blenderobject->bsoft->kVCF; // Velocities correction factor (Baumgarte)
			ci.m_soft_kDP = blenderobject->bsoft->kDP; // Damping coefficient [0,1]

			ci.m_soft_kDG = blenderobject->bsoft->kDG; // Drag coefficient [0,+inf]
			ci.m_soft_kLF = blenderobject->bsoft->kLF; // Lift coefficient [0,+inf]
			ci.m_soft_kPR = blenderobject->bsoft->kPR; // Pressure coefficient [-inf,+inf]
			ci.m_soft_kVC = blenderobject->bsoft->kVC; // Volume conversation coefficient [0,+inf]

			ci.m_soft_kDF = blenderobject->bsoft->kDF; // Dynamic friction coefficient [0,1]
			ci.m_soft_kMT = blenderobject->bsoft->kMT; // Pose matching coefficient [0,1]
			ci.m_soft_kCHR = blenderobject->bsoft->kCHR; // Rigid contacts hardness [0,1]
			ci.m_soft_kKHR = blenderobject->bsoft->kKHR; // Kinetic contacts hardness [0,1]

			ci.m_soft_kSHR = blenderobject->bsoft->kSHR; // Soft contacts hardness [0,1]
			ci.m_soft_kAHR = blenderobject->bsoft->kAHR; // Anchors hardness [0,1]
			ci.m_soft_collisionflags = blenderobject->bsoft->collisionflags; // Vertex/Face or Signed Distance Field(SDF) or Clusters, Soft versus Soft or Rigid
			ci.m_soft_numclusteriterations = blenderobject->bsoft->numclusteriterations; // number of iterations to refine collision clusters

		}
		else {
			ci.m_margin = 0.0f;
			ci.m_gamesoftFlag = OB_BSB_BENDING_CONSTRAINTS | OB_BSB_SHAPE_MATCHING | OB_BSB_AERO_VPOINT;

			ci.m_softBendingDistance = 2;

			ci.m_soft_linStiff = 0.5f;
			ci.m_soft_angStiff = 1.0f; // angular stiffness 0..1
			ci.m_soft_volume = 1.0f; // volume preservation 0..1

			ci.m_soft_viterations = 0;
			ci.m_soft_piterations = 1;
			ci.m_soft_diterations = 0;
			ci.m_soft_citerations = 4;

			ci.m_soft_kSRHR_CL = 0.1f;
			ci.m_soft_kSKHR_CL = 1.0f;
			ci.m_soft_kSSHR_CL = 0.5f;
			ci.m_soft_kSR_SPLT_CL = 0.5f;

			ci.m_soft_kSK_SPLT_CL = 0.5f;
			ci.m_soft_kSS_SPLT_CL = 0.5f;
			ci.m_soft_kVCF = 1;
			ci.m_soft_kDP = 0;

			ci.m_soft_kDG = 0;
			ci.m_soft_kLF = 0;
			ci.m_soft_kPR = 0;
			ci.m_soft_kVC = 0;

			ci.m_soft_kDF = 0.2f;
			ci.m_soft_kMT = 0.05f;
			ci.m_soft_kCHR = 1.0f;
			ci.m_soft_kKHR = 0.1f;

			ci.m_soft_kSHR = 1.0f;
			ci.m_soft_kAHR = 0.7f;
			ci.m_soft_collisionflags = OB_BSB_COL_SDF_RS + OB_BSB_COL_VF_SS;
			ci.m_soft_numclusteriterations = 16;
		}
	}
	else {
		ci.m_margin = blenderobject->margin;
	}

	ci.m_localInertiaTensor = btVector3(ci.m_mass / 3.0f, ci.m_mass / 3.0f, ci.m_mass / 3.0f);

	btCollisionShape *bm = nullptr;

	char bounds = isbulletdyna ? OB_BOUND_SPHERE : OB_BOUND_TRIANGLE_MESH;
	if (!(blenderobject->gameflag & OB_BOUNDS)) {
		if (blenderobject->gameflag & OB_SOFT_BODY) {
			bounds = OB_BOUND_TRIANGLE_MESH;
		}
		else if (blenderobject->gameflag & OB_CHARACTER) {
			bounds = OB_BOUND_SPHERE;
		}
	}
	else {
		if (ELEM(blenderobject->collision_boundtype, OB_BOUND_CONVEX_HULL, OB_BOUND_TRIANGLE_MESH)
		    && blenderobject->type != OB_MESH) {
			// Can't use triangle mesh or convex hull on a non-mesh object, fall-back to sphere
			bounds = OB_BOUND_SPHERE;
		}
		else {
			bounds = blenderobject->collision_boundtype;
		}
	}

	// Get bounds information
	float bounds_center[3], bounds_extends[3];
	BoundBox *bb = BKE_object_boundbox_get(blenderobject);
	if (bb == nullptr) {
		bounds_center[0] = bounds_center[1] = bounds_center[2] = 0.0f;
		bounds_extends[0] = bounds_extends[1] = bounds_extends[2] = 1.0f;
	}
	else {
		bounds_extends[0] = 0.5f * fabsf(bb->vec[0][0] - bb->vec[4][0]);
		bounds_extends[1] = 0.5f * fabsf(bb->vec[0][1] - bb->vec[2][1]);
		bounds_extends[2] = 0.5f * fabsf(bb->vec[0][2] - bb->vec[1][2]);

		bounds_center[0] = 0.5f * (bb->vec[0][0] + bb->vec[4][0]);
		bounds_center[1] = 0.5f * (bb->vec[0][1] + bb->vec[2][1]);
		bounds_center[2] = 0.5f * (bb->vec[0][2] + bb->vec[1][2]);
	}

	switch (bounds) {
		case OB_BOUND_SPHERE:
		{
			shapeInfo->m_shapeType = PHY_SHAPE_SPHERE;
			bm = shapeInfo->CreateBulletShape(ci.m_margin);
			break;
		}
		case OB_BOUND_BOX:
		{
			shapeInfo->m_halfExtend.setValue(
				2.0f * bounds_extends[0],
				2.0f * bounds_extends[1],
				2.0f * bounds_extends[2]);

			shapeInfo->m_halfExtend /= 2.0f;
			shapeInfo->m_halfExtend = shapeInfo->m_halfExtend.absolute();
			shapeInfo->m_shapeType = PHY_SHAPE_BOX;
			bm = shapeInfo->CreateBulletShape(ci.m_margin);
			break;
		}
		case OB_BOUND_CYLINDER:
		{
			float radius = std::max(bounds_extends[0], bounds_extends[1]);
			shapeInfo->m_halfExtend.setValue(
				radius,
				radius,
				bounds_extends[2]
				);
			shapeInfo->m_shapeType = PHY_SHAPE_CYLINDER;
			bm = shapeInfo->CreateBulletShape(ci.m_margin);
			break;
		}

		case OB_BOUND_CONE:
		{
			shapeInfo->m_radius = std::max(bounds_extends[0], bounds_extends[1]);
			shapeInfo->m_height = 2.0f * bounds_extends[2];
			shapeInfo->m_shapeType = PHY_SHAPE_CONE;
			bm = shapeInfo->CreateBulletShape(ci.m_margin);
			break;
		}
		case OB_BOUND_CONVEX_HULL:
		{
			// Convex shapes can be shared, check first if we already have a shape on that mesh.
			CcdShapeConstructionInfo *sharedShapeInfo = CcdShapeConstructionInfo::FindMesh(meshobj, gameobj->GetDeformer(), PHY_SHAPE_POLYTOPE);
			if (sharedShapeInfo) {
				shapeInfo->Release();
				shapeInfo = sharedShapeInfo;
				shapeInfo->AddRef();
			}
			else {
				shapeInfo->m_shapeType = PHY_SHAPE_POLYTOPE;
				// Update from deformer or mesh.
				shapeInfo->UpdateMesh(gameobj, nullptr);
			}

			bm = shapeInfo->CreateBulletShape(ci.m_margin);
			break;
		}
		case OB_BOUND_CAPSULE:
		{
			shapeInfo->m_radius = std::max(bounds_extends[0], bounds_extends[1]);
			shapeInfo->m_height = 2.0f * bounds_extends[2];
			if (shapeInfo->m_height < 0.0f) {
				shapeInfo->m_height = 0.0f;
			}
			shapeInfo->m_shapeType = PHY_SHAPE_CAPSULE;
			bm = shapeInfo->CreateBulletShape(ci.m_margin);
			break;
		}
		case OB_BOUND_TRIANGLE_MESH:
		{
			// Mesh shapes can be shared, check first if we already have a shape on that mesh.
			CcdShapeConstructionInfo *sharedShapeInfo = CcdShapeConstructionInfo::FindMesh(meshobj, gameobj->GetDeformer(), PHY_SHAPE_MESH);
			if (sharedShapeInfo) {
				shapeInfo->Release();
				shapeInfo = sharedShapeInfo;
				shapeInfo->AddRef();
			}
			else {
				shapeInfo->m_shapeType = PHY_SHAPE_MESH;
				// Update from deformer or mesh.
				shapeInfo->UpdateMesh(gameobj, nullptr);
			}

			// Soft bodies can benefit from welding, don't do it on non-soft bodies
			if (isbulletsoftbody) {
				// disable welding: it doesn't bring any additional stability and it breaks the relation between soft body collision shape and graphic mesh
				// shapeInfo->setVertexWeldingThreshold1((blenderobject->bsoft) ? blenderobject->bsoft->welding ? 0.f);
				shapeInfo->setVertexWeldingThreshold1(0.0f); //todo: expose this to the UI
			}

			bm = shapeInfo->CreateBulletShape(ci.m_margin, useGimpact, !isbulletsoftbody);
			//should we compute inertia for dynamic shape?
			//bm->calculateLocalInertia(ci.m_mass,ci.m_localInertiaTensor);

			break;
		}
		case OB_BOUND_EMPTY:
		{
			shapeInfo->m_shapeType = PHY_SHAPE_EMPTY;
			bm = shapeInfo->CreateBulletShape(ci.m_margin);
			break;
		}
	}

	if (!bm) {
		delete motionstate;
		shapeInfo->Release();
		return;
	}

	if (isCompoundChild) {
		//find parent, compound shape and add to it
		//take relative transform into account!
		CcdPhysicsController *parentCtrl = (CcdPhysicsController *)compoundParent->GetPhysicsController();
		BLI_assert(parentCtrl);

		// only makes compound shape if parent has a physics controller (i.e not an empty, etc)
		if (parentCtrl) {
			CcdShapeConstructionInfo *parentShapeInfo = parentCtrl->GetShapeInfo();
			btRigidBody *rigidbody = parentCtrl->GetRigidBody();
			btCollisionShape *colShape = rigidbody->getCollisionShape();
			BLI_assert(colShape->isCompound());
			btCompoundShape *compoundShape = (btCompoundShape *)colShape;

			// compute the local transform from parent, this may include several node in the chain
			SG_Node *gameNode = gameobj->GetNode();
			SG_Node *parentNode = compoundParent->GetNode();
			// relative transform
			mt::vec3 parentScale = parentNode->GetWorldScaling();
			parentScale[0] = 1.0f / parentScale[0];
			parentScale[1] = 1.0f / parentScale[1];
			parentScale[2] = 1.0f / parentScale[2];
			mt::vec3 relativeScale = gameNode->GetWorldScaling() * parentScale;
			mt::mat3 parentInvRot = parentNode->GetWorldOrientation().Transpose();
			mt::vec3 relativePos = parentInvRot * ((gameNode->GetWorldPosition() - parentNode->GetWorldPosition()) * parentScale);
			mt::mat3 relativeRot = parentInvRot * gameNode->GetWorldOrientation();

			shapeInfo->m_childScale = ToBullet(relativeScale);
			bm->setLocalScaling(shapeInfo->m_childScale);
			shapeInfo->m_childTrans.setOrigin(ToBullet(relativePos));
			shapeInfo->m_childTrans.setBasis(ToBullet(relativeRot));

			parentShapeInfo->AddShape(shapeInfo);
			compoundShape->addChildShape(shapeInfo->m_childTrans, bm);

			// Recalculate inertia for object owning compound shape.
			if (!rigidbody->isStaticOrKinematicObject()) {
				btVector3 localInertia;
				const float mass = 1.0f / rigidbody->getInvMass();
				compoundShape->calculateLocalInertia(mass, localInertia);
				rigidbody->setMassProps(mass, localInertia * parentCtrl->GetInertiaFactor());
			}
			shapeInfo->Release();
			// delete motionstate as it's not used
			delete motionstate;
		}
		return;
	}

	if (hasCompoundChildren) {
		// create a compound shape info
		CcdShapeConstructionInfo *compoundShapeInfo = new CcdShapeConstructionInfo();
		compoundShapeInfo->m_shapeType = PHY_SHAPE_COMPOUND;
		compoundShapeInfo->AddShape(shapeInfo);
		// create the compound shape manually as we already have the child shape
		btCompoundShape *compoundShape = new btCompoundShape();
		compoundShape->addChildShape(shapeInfo->m_childTrans, bm);
		compoundShape->setUserPointer(compoundShapeInfo);
		// now replace the shape
		bm = compoundShape;
		shapeInfo->Release();
		shapeInfo = compoundShapeInfo;
	}

#ifdef TEST_SIMD_HULL
	if (bm->IsPolyhedral()) {
		PolyhedralConvexShape *polyhedron = static_cast<PolyhedralConvexShape *>(bm);
		if (!polyhedron->m_optionalHull) {
			//first convert vertices in 'Point3' format
			int numPoints = polyhedron->GetNumVertices();
			Point3 *points = new Point3[numPoints + 1];
			//first 4 points should not be co-planar, so add central point to satisfy MakeHull
			points[0] = Point3(0.0f, 0.0f, 0.0f);

			btVector3 vertex;
			for (int p = 0; p < numPoints; p++) {
				polyhedron->GetVertex(p, vertex);
				points[p + 1] = Point3(vertex.getX(), vertex.getY(), vertex.getZ());
			}

			Hull *hull = Hull::MakeHull(numPoints + 1, points);
			polyhedron->m_optionalHull = hull;
		}
	}
#endif //TEST_SIMD_HULL


	ci.m_collisionShape = bm;
	ci.m_shapeInfo = shapeInfo;
	ci.m_friction = blenderobject->friction;
	ci.m_rollingFriction = blenderobject->rolling_friction;
	ci.m_restitution = blenderobject->reflect;
	ci.m_physicsEnv = this;
	ci.m_linearDamping = blenderobject->damping;
	ci.m_angularDamping = blenderobject->rdamping;
	//need a bit of damping, else system doesn't behave well
	ci.m_inertiaFactor = blenderobject->formfactor / 0.4f;//defaults to 0.4, don't want to change behavior

	ci.m_do_anisotropic = (blenderobject->gameflag & OB_ANISOTROPIC_FRICTION);
	ci.m_anisotropicFriction = btVector3(
		blenderobject->anisotropicFriction[0],
		blenderobject->anisotropicFriction[1],
		blenderobject->anisotropicFriction[2]);

	//do Fh, do Rot Fh
	ci.m_do_fh = (blenderobject->gameflag & OB_DO_FH);
	ci.m_do_rot_fh = (blenderobject->gameflag & OB_ROT_FH);
	ci.m_fh_damping = blenderobject->xyfrict;
	ci.m_fh_distance = blenderobject->fhdist;
	ci.m_fh_normal = (blenderobject->dynamode & OB_FH_NOR);
	ci.m_fh_spring = blenderobject->fh;

	ci.m_collisionFilterGroup =
		(isbulletsensor) ? short(CcdConstructionInfo::SensorFilter) :
		(isbulletdyna) ? short(CcdConstructionInfo::DynamicFilter) :
		(isbulletchar) ? short(CcdConstructionInfo::CharacterFilter) :
		short(CcdConstructionInfo::StaticFilter);
	ci.m_collisionFilterMask =
		(isbulletsensor) ? short(CcdConstructionInfo::AllFilter ^ CcdConstructionInfo::SensorFilter) :
		(isbulletdyna) ? short(CcdConstructionInfo::AllFilter) :
		(isbulletchar) ? short(CcdConstructionInfo::AllFilter) :
		short(CcdConstructionInfo::AllFilter ^ CcdConstructionInfo::StaticFilter);
	ci.m_bRigid = isbulletdyna && isbulletrigidbody;
	ci.m_bSoft = isbulletsoftbody;
	ci.m_bDyna = isbulletdyna;
	ci.m_bSensor = isbulletsensor;
	ci.m_bCharacter = isbulletchar;
	ci.m_bGimpact = useGimpact;
	mt::vec3 scaling = gameobj->NodeGetWorldScaling();
	ci.m_scaling.setValue(scaling[0], scaling[1], scaling[2]);
	CcdPhysicsController *physicscontroller = new CcdPhysicsController(ci);
	// shapeInfo is reference counted, decrement now as we don't use it anymore
	if (shapeInfo) {
		shapeInfo->Release();
	}

	gameobj->SetPhysicsController(physicscontroller);

	physicscontroller->SetNewClientInfo(&gameobj->GetClientInfo());

	// don't add automatically sensor object, they are added when a collision sensor is registered
	if (!isbulletsensor && (blenderobject->lay & activeLayerBitInfo) != 0) {
		this->AddCcdPhysicsController(physicscontroller);
	}

	{
		btRigidBody *rbody = physicscontroller->GetRigidBody();

		if (rbody) {
			rbody->setLinearFactor(ci.m_linearFactor);

			if (isbulletrigidbody) {
				rbody->setAngularFactor(ci.m_angularFactor);
			}

			if (rbody && (blenderobject->gameflag & OB_COLLISION_RESPONSE) != 0) {
				rbody->setActivationState(DISABLE_DEACTIVATION);
			}
		}
	}

	if (parentRoot) {
		physicscontroller->SuspendDynamics(false);
	}

	CcdPhysicsController *parentCtrl = parentRoot ? static_cast<CcdPhysicsController *>(parentRoot->GetPhysicsController()) : nullptr;
	physicscontroller->SetParentRoot(parentCtrl);
}

void CcdPhysicsEnvironment::SetupObjectConstraints(KX_GameObject *obj_src, KX_GameObject *obj_dest,
                                                   bRigidBodyJointConstraint *dat)
{
	PHY_IPhysicsController *phy_src = obj_src->GetPhysicsController();
	PHY_IPhysicsController *phy_dest = obj_dest->GetPhysicsController();
	PHY_IPhysicsEnvironment *phys_env = obj_src->GetScene()->GetPhysicsEnvironment();

	/* We need to pass a full constraint frame, not just axis. */
	mt::mat3 localCFrame(mt::vec3(dat->axX, dat->axY, dat->axZ));
	mt::vec3 axis0 = localCFrame.GetColumn(0);
	mt::vec3 axis1 = localCFrame.GetColumn(1);
	mt::vec3 axis2 = localCFrame.GetColumn(2);
	mt::vec3 scale = obj_src->NodeGetWorldScaling();

	/* Apply not only the pivot and axis values, but also take scale into count
	 * this is not working well, if only one or two axis are scaled, but works ok on
	 * homogeneous scaling. */
	PHY_IConstraint *constraint = phys_env->CreateConstraint(
		phy_src, phy_dest, (PHY_ConstraintType)dat->type,
		(float)(dat->pivX * scale.x), (float)(dat->pivY * scale.y), (float)(dat->pivZ * scale.z),
		(float)(axis0.x * scale.x), (float)(axis0.y * scale.y), (float)(axis0.z * scale.z),
		(float)(axis1.x * scale.x), (float)(axis1.y * scale.y), (float)(axis1.z * scale.z),
		(float)(axis2.x * scale.x), (float)(axis2.y * scale.y), (float)(axis2.z * scale.z),
		dat->flag);

	/* PHY_POINT2POINT_CONSTRAINT = 1,
	 * PHY_LINEHINGE_CONSTRAINT = 2,
	 * PHY_ANGULAR_CONSTRAINT = 3,
	 * PHY_CONE_TWIST_CONSTRAINT = 4,
	 * PHY_VEHICLE_CONSTRAINT = 11,
	 * PHY_GENERIC_6DOF_CONSTRAINT = 12 */

	if (!constraint) {
		return;
	}

	int dof = 0;
	int dof_max = 0;
	int dofbit = 0;

	switch (dat->type) {
		/* Set all the limits for generic 6DOF constraint. */
		case PHY_GENERIC_6DOF_CONSTRAINT:
		{
			dof_max = 6;
			dofbit = 1;
			break;
		}
		/* Set XYZ angular limits for cone twist constraint. */
		case PHY_CONE_TWIST_CONSTRAINT:
		{
			dof = 3;
			dof_max = 6;
			dofbit = 1 << 3;
			break;
		}
		/* Set only X angular limits for line hinge and angular constraint. */
		case PHY_LINEHINGE_CONSTRAINT:
		case PHY_ANGULAR_CONSTRAINT:
		{
			dof = 3;
			dof_max = 4;
			dofbit = 1 << 3;
			break;
		}
		default:
		{
			break;
		}
	}

	for (; dof < dof_max; dof++) {
		if (dat->flag & dofbit) {
			constraint->SetParam(dof, dat->minLimit[dof], dat->maxLimit[dof]);
		}
		else {
			/* minLimit > maxLimit means free (no limit) for this degree of freedom. */
			constraint->SetParam(dof, 1.0f, -1.0f);
		}
		dofbit <<= 1;
	}

	if (dat->flag & CONSTRAINT_USE_BREAKING) {
		constraint->SetBreakingThreshold(dat->breaking);
	}
}

CcdCollData::CcdCollData(const btPersistentManifold *manifoldPoint)
	:m_manifoldPoint(manifoldPoint)
{
}

CcdCollData::~CcdCollData()
{
}

unsigned int CcdCollData::GetNumContacts() const
{
	return m_manifoldPoint->getNumContacts();
}

mt::vec3 CcdCollData::GetLocalPointA(unsigned int index, bool first) const
{
	const btManifoldPoint& point = m_manifoldPoint->getContactPoint(index);
	return ToMt(first ? point.m_localPointA : point.m_localPointB);
}

mt::vec3 CcdCollData::GetLocalPointB(unsigned int index, bool first) const
{
	const btManifoldPoint& point = m_manifoldPoint->getContactPoint(index);
	return ToMt(first ? point.m_localPointB : point.m_localPointA);
}

mt::vec3 CcdCollData::GetWorldPoint(unsigned int index, bool first) const
{
	const btManifoldPoint& point = m_manifoldPoint->getContactPoint(index);
	return ToMt(point.m_positionWorldOnB);
}

mt::vec3 CcdCollData::GetNormal(unsigned int index, bool first) const
{
	const btManifoldPoint& point = m_manifoldPoint->getContactPoint(index);
	return ToMt(first ? -point.m_normalWorldOnB : point.m_normalWorldOnB);
}

float CcdCollData::GetCombinedFriction(unsigned int index, bool first) const
{
	const btManifoldPoint& point = m_manifoldPoint->getContactPoint(index);
	return point.m_combinedFriction;
}

float CcdCollData::GetCombinedRollingFriction(unsigned int index, bool first) const
{
	const btManifoldPoint& point = m_manifoldPoint->getContactPoint(index);
	return point.m_combinedRollingFriction;
}

float CcdCollData::GetCombinedRestitution(unsigned int index, bool first) const
{
	const btManifoldPoint& point = m_manifoldPoint->getContactPoint(index);
	return point.m_combinedRestitution;
}

float CcdCollData::GetAppliedImpulse(unsigned int index, bool first) const
{
	const btManifoldPoint& point = m_manifoldPoint->getContactPoint(index);
	return point.m_appliedImpulse;
}
