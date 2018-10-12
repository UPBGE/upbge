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

/** \file CcdPhysicsEnvironment.h
 *  \ingroup physbullet
 *  See also \ref bulletdoc
 */

#ifndef __CCDPHYSICSENVIRONMENT_H__
#define __CCDPHYSICSENVIRONMENT_H__

#include "PHY_IPhysicsEnvironment.h"
#include "KX_KetsjiEngine.h"
#include "KX_Globals.h"

#include "CcdPhysicsController.h"

#include <vector>
#include <set>
#include <map>
#include "LinearMath/btVector3.h"
#include "LinearMath/btTransform.h"
#include "BulletDynamics/ConstraintSolver/btContactSolverInfo.h"

class PHY_IVehicle;
class CcdGraphicController;
class WrapperVehicle;
class CcdOverlapFilterCallBack;
class CcdShapeConstructionInfo;

class btTypedConstraint;
class btSimulationIslandManager;
class btCollisionDispatcher;
class btDispatcher;
class btDefaultCollisionConfiguration;
class btPersistentManifold;
class btBroadphaseInterface;
struct btDbvtBroadphase;
class btGhostPairCallback;
class btOverlappingPairCache;
class btIDebugDraw;
class btDynamicsWorld;
class btSoftRigidDynamicsWorldMt;
class btConstraintSolver;
class btConstraintSolverPoolMt;

/// Find the id of the closest node to a point in a soft body.
int Ccd_FindClosestNode(btSoftBody *sb, const btVector3& worldPoint);

class CcdDebugDraw : public btIDebugDraw
{
private:
	int m_debugMode;

public:
	CcdDebugDraw();

	virtual void drawLine(const btVector3& from, const btVector3& to, const btVector3& color);
	virtual void reportErrorWarning(const char *warningString);
	virtual void drawContactPoint(const btVector3& PointOnB, const btVector3& normalOnB, float distance, int lifeTime, const btVector3& color);
	virtual void setDebugMode(int debugMode);
	virtual int getDebugMode() const;
	virtual void draw3dText(const btVector3& location, const char *textString);
};

/** CcdPhysicsEnvironment is an experimental mainloop for physics simulation using optional continuous collision detection.
 * Physics Environment takes care of stepping the simulation and is a container for physics entities.
 * It stores rigidbodies,constraints, materials etc.
 * A derived class may be able to 'construct' entities by loading and/or converting
 */
class CcdPhysicsEnvironment : public PHY_IPhysicsEnvironment, public mt::SimdClassAllocator
{
	friend class CcdOverlapFilterCallBack;

	/// Removes the constraint and his references from the owner and the target.
	void RemoveConstraint(btTypedConstraint *con, bool free);
	/// Remove a vehicle wrapper.
	void RemoveVehicle(WrapperVehicle *vehicle, bool free);
	/// Remove vehicle wrapper used by a physics controller used as chassis.
	void RemoveVehicle(CcdPhysicsController *ctrl, bool free);
	/// Restore the constraint if the owner and target are presents.
	void RestoreConstraint(CcdPhysicsController *ctrl, btTypedConstraint *con);

protected:
	btVector3 m_gravity;

	CcdDebugDraw m_debugDrawer;

	std::unique_ptr<btDefaultCollisionConfiguration> m_collisionConfiguration;
	/// broadphase for dynamic world
	std::unique_ptr<btBroadphaseInterface> m_broadphase;
	/// for culling only
	std::unique_ptr<btOverlappingPairCache> m_cullingCache;
	/// broadphase for culling
	std::unique_ptr<btDbvtBroadphase> m_cullingTree;

	/** use explicit btSoftRigidDynamicsWorld/btDiscreteDynamicsWorld* so that we have access to
	 * btDiscreteDynamicsWorld::addRigidBody(body,filter,group)
	 * so that we can set the body collision filter/group at the time of creation
	 * and not afterwards (breaks the collision system for radar/near sensor)
	 * Ideally we would like to have access to this function from the btDynamicsWorld interface
	 */
	std::unique_ptr<btSoftRigidDynamicsWorldMt> m_dynamicsWorld;

	std::unique_ptr<btConstraintSolverPoolMt> m_solverPool;
	std::vector<btConstraintSolver *> m_solvers;

	std::unique_ptr<CcdOverlapFilterCallBack> m_filterCallback;

	std::unique_ptr<btGhostPairCallback> m_ghostPairCallback;

	std::unique_ptr<btCollisionDispatcher> m_dispatcher;

	std::set<CcdPhysicsController *> m_controllers;
	std::vector<WrapperVehicle *> m_wrapperVehicles;

	PHY_ResponseCallback m_triggerCallbacks[PHY_NUM_RESPONSE];
	void *m_triggerCallbacksUserPtrs[PHY_NUM_RESPONSE];

	/// solver iterations
	int m_numIterations;

	/// timestep subdivisions
	int m_numTimeSubSteps;

	int m_ccdMode;
	PHY_SolverType m_solverType;

	float m_deactivationTime;
	float m_linearDeactivationThreshold;
	float m_angularDeactivationThreshold;
	float m_contactBreakingThreshold;

	void ProcessFhSprings(double curTime, float timeStep);
	virtual void ExportFile(const std::string& filename);

public:
	CcdPhysicsEnvironment(PHY_SolverType solverType, bool useDbvtCulling);

	virtual ~CcdPhysicsEnvironment();

	/////////////////////////////////////
	//PHY_IPhysicsEnvironment interface
	/////////////////////////////////////

	/// Perform an integration step of duration 'timeStep'.

	virtual void SetNumIterations(int numIter);
	virtual void SetNumTimeSubSteps(int numTimeSubSteps)
	{
		m_numTimeSubSteps = numTimeSubSteps;
	}
	virtual void SetDeactivationTime(float dTime);
	virtual void SetDeactivationLinearTreshold(float linTresh);
	virtual void SetDeactivationAngularTreshold(float angTresh);
	virtual void SetContactBreakingTreshold(float contactBreakingTreshold);
	virtual void SetCcdMode(int ccdMode);
	virtual void SetSolverType(PHY_SolverType solverType);
	virtual void SetSolverSorConstant(float sor);
	virtual void SetSolverTau(float tau);
	virtual void SetSolverDamping(float damping);
	virtual void SetLinearAirDamping(float damping);
	virtual void SetUseEpa(bool epa);

	virtual int GetNumTimeSubSteps()
	{
		return m_numTimeSubSteps;
	}
	/// Perform an integration step of duration 'timeStep'.
	virtual bool ProceedDeltaTime(double curTime, float timeStep, float interval);

	/**
	 * Called by Bullet for every physical simulation (sub)tick.
	 * Our constructor registers this callback to Bullet, which stores a pointer to 'this' in
	 * the btDynamicsWorld::getWorldUserInfo() pointer.
	 */
	static void StaticSimulationSubtickCallback(btDynamicsWorld *world, btScalar timeStep);
	void SimulationSubtickCallback(btScalar timeStep);

	virtual void DebugDrawWorld();

	virtual void SetFixedTimeStep(bool useFixedTimeStep, float fixedTimeStep)
	{
		SetNumTimeSubSteps((int)(fixedTimeStep / KX_GetActiveEngine()->GetTicRate()));
	}
	/// returns 0.f if no fixed timestep is used
	virtual float GetFixedTimeStep()
	{
		return 0.f;
	}

	virtual void SetDebugMode(int debugMode);
	virtual int GetDebugMode() const;

	virtual void SetGravity(float x, float y, float z);
	virtual mt::vec3 GetGravity() const;


	virtual PHY_IConstraint *CreateConstraint(class PHY_IPhysicsController *ctrl, class PHY_IPhysicsController *ctrl2, PHY_ConstraintType type,
								 float pivotX, float pivotY, float pivotZ,
								 float axisX, float axisY, float axisZ,
								 float axis1X = 0, float axis1Y = 0, float axis1Z = 0,
								 float axis2X = 0, float axis2Y = 0, float axis2Z = 0, int flag = 0);
	virtual PHY_IVehicle *CreateVehicle(PHY_IPhysicsController *ctrl);

	virtual void RemoveConstraintById(int constraintid, bool free);

	virtual float getAppliedImpulse(int constraintid);

	virtual void CallbackTriggers();

	//complex constraint for vehicles
	virtual PHY_IVehicle *GetVehicleConstraint(int constraintId);
	    // Character physics wrapper
	virtual PHY_ICharacter *GetCharacterController(class KX_GameObject *ob);

	btTypedConstraint *GetConstraintById(int constraintId);

	virtual PHY_IPhysicsController *RayTest(PHY_IRayCastFilterCallback &filterCallback, float fromX, float fromY, float fromZ, float toX, float toY, float toZ);
	virtual bool CullingTest(PHY_CullingCallback callback, void *userData, const std::array<mt::vec4, 6>& planes,
							 int occlusionRes, const int *viewport, const mt::mat4& matrix);


	//Methods for gamelogic collision/physics callbacks
	virtual void AddSensor(PHY_IPhysicsController *ctrl);
	virtual void RemoveSensor(PHY_IPhysicsController *ctrl);
	virtual void AddCollisionCallback(int response_class, PHY_ResponseCallback callback, void *user);
	virtual bool RequestCollisionCallback(PHY_IPhysicsController *ctrl);
	virtual bool RemoveCollisionCallback(PHY_IPhysicsController *ctrl);
	virtual PHY_CollisionTestResult CheckCollision(PHY_IPhysicsController *ctrl0, PHY_IPhysicsController *ctrl1);
	//These two methods are used *solely* to create controllers for Near/Radar sensor! Don't use for anything else
	virtual PHY_IPhysicsController *CreateSphereController(float radius, const mt::vec3& position);
	virtual PHY_IPhysicsController *CreateConeController(float coneradius, float coneheight);

	virtual int GetNumContactPoints();

	virtual void GetContactPoint(int i, float& hitX, float& hitY, float& hitZ, float& normalX, float& normalY, float& normalZ);

	//////////////////////
	//CcdPhysicsEnvironment interface
	////////////////////////

	void AddCcdPhysicsController(CcdPhysicsController *ctrl);

	bool RemoveCcdPhysicsController(CcdPhysicsController *ctrl, bool freeConstraints);

	void UpdateCcdPhysicsController(CcdPhysicsController *ctrl, btScalar newMass, int newCollisionFlags, short int newCollisionGroup, short int newCollisionMask);

	void RefreshCcdPhysicsController(CcdPhysicsController *ctrl);

	bool IsActiveCcdPhysicsController(CcdPhysicsController *ctrl);

	void AddCcdGraphicController(CcdGraphicController *ctrl);

	void RemoveCcdGraphicController(CcdGraphicController *ctrl);

	/**
	 * Update all physics controllers shape which use the same shape construction info.
	 * Call RecreateControllerShape on controllers which use the same shape
	 * construction info that argument shapeInfo.
	 * You need to call this function when the shape construction info changed.
	 */
	void UpdateCcdPhysicsControllerShape(CcdShapeConstructionInfo *shapeInfo);

	btBroadphaseInterface *GetBroadphase();
	btDbvtBroadphase *GetCullingTree()
	{
		return m_cullingTree.get();
	}

	btDispatcher *GetDispatcher();

	const btPersistentManifold *GetManifold(int index) const;

	void SyncMotionStates(float timeStep);

	btSoftRigidDynamicsWorldMt *GetDynamicsWorld()
	{
		return m_dynamicsWorld.get();
	}

	btConstraintSolver *GetConstraintSolver();

	void MergeEnvironment(PHY_IPhysicsEnvironment *other_env);

	static CcdPhysicsEnvironment *Create(struct Scene *blenderscene, bool visualizePhysics);

	virtual void ConvertObject(BL_SceneConverter& converter,
							   KX_GameObject *gameobj,
	                           RAS_Mesh *meshobj,
	                           KX_Scene *kxscene,
	                           PHY_IMotionState *motionstate,
	                           int activeLayerBitInfo,
	                           bool isCompoundChild,
	                           bool hasCompoundChildren);

	/* Set the rigid body joints constraints values for converted objects and replicated group instances. */
	virtual void SetupObjectConstraints(KX_GameObject *obj_src, KX_GameObject *obj_dest,
	                                    bRigidBodyJointConstraint *dat);
};

class CcdCollData : public PHY_ICollData
{
	const btPersistentManifold *m_manifoldPoint;
public:
	CcdCollData(const btPersistentManifold *manifoldPoint);
	virtual ~CcdCollData();

	virtual unsigned int GetNumContacts() const;
	virtual mt::vec3 GetLocalPointA(unsigned int index, bool first) const;
	virtual mt::vec3 GetLocalPointB(unsigned int index, bool first) const;
	virtual mt::vec3 GetWorldPoint(unsigned int index, bool first) const;
	virtual mt::vec3 GetNormal(unsigned int index, bool first) const;
	virtual float GetCombinedFriction(unsigned int index, bool first) const;
	virtual float GetCombinedRollingFriction(unsigned int index, bool first) const;
	virtual float GetCombinedRestitution(unsigned int index, bool first) const;
	virtual float GetAppliedImpulse(unsigned int index, bool first) const;
};

#endif  /* __CCDPHYSICSENVIRONMENT_H__ */
