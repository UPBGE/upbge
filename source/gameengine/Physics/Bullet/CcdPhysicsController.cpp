/** \file gameengine/Physics/Bullet/CcdPhysicsController.cpp
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

#ifndef WIN32
#include <stdint.h>
#endif

#include "CM_Message.h"

#include "CcdPhysicsController.h"
#include "btBulletDynamicsCommon.h"
#include "BulletCollision/CollisionDispatch/btGhostObject.h"
#include "BulletCollision/CollisionShapes/btScaledBvhTriangleMeshShape.h"
#include "BulletCollision/CollisionShapes/btTriangleIndexVertexArray.h"

#include "PHY_IMotionState.h"
#include "CcdPhysicsEnvironment.h"

#include "RAS_Deformer.h"
#include "RAS_IPolygonMaterial.h"
#include "RAS_MaterialBucket.h"

#include "KX_GameObject.h"
#include "KX_Mesh.h"

#include "BulletSoftBody/btSoftBody.h"
#include "BulletSoftBody/btSoftBodyInternals.h"
#include "BulletSoftBody/btSoftBodyHelpers.h"
#include "LinearMath/btConvexHull.h"
#include "BulletCollision/Gimpact/btGImpactShape.h"

#include "BulletSoftBody/btSoftRigidDynamicsWorldMt.h"

#include "BLI_utildefines.h"

/// todo: fill all the empty CcdPhysicsController methods, hook them up to the btRigidBody class

//'temporarily' global variables
extern float gDeactivationTime;
extern bool gDisableDeactivation;

float gLinearSleepingTreshold;
float gAngularSleepingTreshold;

CcdCharacter::CcdCharacter(CcdPhysicsController *ctrl, btMotionState *motionState,
                           btPairCachingGhostObject *ghost, btConvexShape *shape, float stepHeight)
	:btKinematicCharacterController(ghost, shape, stepHeight, btVector3(0.0f, 0.0f, 1.0f)),
	m_ctrl(ctrl),
	m_motionState(motionState),
	m_jumps(0),
	m_maxJumps(1)
{
}

void CcdCharacter::updateAction(btCollisionWorld *collisionWorld, btScalar dt)
{
	if (onGround()) {
		m_jumps = 0;
	}

	btKinematicCharacterController::updateAction(collisionWorld, dt);
	m_motionState->setWorldTransform(getGhostObject()->getWorldTransform());
}

unsigned char CcdCharacter::getMaxJumps() const
{
	return m_maxJumps;
}

void CcdCharacter::setMaxJumps(unsigned char maxJumps)
{
	m_maxJumps = maxJumps;
}

unsigned char CcdCharacter::getJumpCount() const
{
	return m_jumps;
}

bool CcdCharacter::canJump() const
{
	return (onGround() && m_maxJumps > 0) || m_jumps < m_maxJumps;
}

void CcdCharacter::jump()
{
	if (!canJump()) {
		return;
	}

	m_verticalVelocity = m_jumpSpeed;
	m_wasJumping = true;
	m_jumps++;
}

const btVector3& CcdCharacter::getWalkDirection()
{
	return m_walkDirection;
}

float CcdCharacter::GetFallSpeed() const
{
	return m_fallSpeed;
}

void CcdCharacter::SetFallSpeed(float fallSpeed)
{
	setFallSpeed(fallSpeed);
}

float CcdCharacter::GetMaxSlope() const
{
	return m_maxSlopeRadians;
}

void CcdCharacter::SetMaxSlope(float maxSlope)
{
	setMaxSlope(maxSlope);
}

float CcdCharacter::GetJumpSpeed() const
{
	return m_jumpSpeed;
}

void CcdCharacter::SetJumpSpeed(float jumpSpeed)
{
	setJumpSpeed(jumpSpeed);
}

void CcdCharacter::SetVelocity(const btVector3& vel, float time, bool local)
{
	btVector3 v = vel;
	if (local) {
		const btTransform xform = getGhostObject()->getWorldTransform();
		v = xform.getBasis() * v;
	}

	// Avoid changing velocity and keeping previous time interval.
	m_velocityTimeInterval = 0.0f;

	setVelocityForTimeInterval(v, time);
}

void CcdCharacter::SetVelocity(const mt::vec3& vel, float time, bool local)
{
	SetVelocity(ToBullet(vel), time, local);
}

void CcdCharacter::Reset()
{
	btCollisionWorld *world = m_ctrl->GetPhysicsEnvironment()->GetDynamicsWorld();
	reset(world);
}

bool CleanPairCallback::processOverlap(btBroadphasePair &pair)
{
	if ((pair.m_pProxy0 == m_cleanProxy) || (pair.m_pProxy1 == m_cleanProxy)) {
		m_pairCache->cleanOverlappingPair(pair, m_dispatcher);
		CcdPhysicsController *ctrl0 = (CcdPhysicsController *)(((btCollisionObject *)pair.m_pProxy0->m_clientObject)->getUserPointer());
		CcdPhysicsController *ctrl1 = (CcdPhysicsController *)(((btCollisionObject *)pair.m_pProxy1->m_clientObject)->getUserPointer());
		ctrl0->GetCollisionObject()->activate(false);
		ctrl1->GetCollisionObject()->activate(false);
	}
	return false;
}

CcdPhysicsController::CcdPhysicsController(const CcdConstructionInfo& ci)
	:m_cci(ci)
{
	m_newClientInfo = 0;
	m_registerCount = 0;
	m_softBodyTransformInitialized = false;
	m_parentRoot = nullptr;
	// copy pointers locally to allow smart release
	m_MotionState = ci.m_MotionState;
	m_collisionShape = ci.m_collisionShape;
	// apply scaling before creating rigid body
	m_collisionShape->setLocalScaling(m_cci.m_scaling);
	if (m_cci.m_mass) {
		m_collisionShape->calculateLocalInertia(m_cci.m_mass, m_cci.m_localInertiaTensor);
	}
	// shape info is shared, increment ref count
	m_shapeInfo = ci.m_shapeInfo;
	if (m_shapeInfo) {
		m_shapeInfo->AddRef();
	}

	m_bulletChildShape = nullptr;

	m_bulletMotionState = 0;
	m_characterController = 0;
	m_savedCollisionFlags = 0;
	m_savedCollisionFilterGroup = 0;
	m_savedCollisionFilterMask = 0;
	m_savedMass = 0.0f;
	m_savedDyna = false;
	m_suspended = false;

	CreateRigidbody();
}

void CcdPhysicsController::addCcdConstraintRef(btTypedConstraint *c)
{
	int index = m_ccdConstraintRefs.findLinearSearch(c);
	if (index == m_ccdConstraintRefs.size()) {
		m_ccdConstraintRefs.push_back(c);
	}
}

void CcdPhysicsController::removeCcdConstraintRef(btTypedConstraint *c)
{
	m_ccdConstraintRefs.remove(c);
}

btTypedConstraint *CcdPhysicsController::getCcdConstraintRef(int index)
{
	return m_ccdConstraintRefs[index];
}

int CcdPhysicsController::getNumCcdConstraintRefs() const
{
	return m_ccdConstraintRefs.size();
}

btTransform CcdPhysicsController::GetTransformFromMotionState(PHY_IMotionState *motionState)
{
	const mt::vec3 pos = motionState->GetWorldPosition();
	const mt::mat3 mat = motionState->GetWorldOrientation();

	return btTransform(ToBullet(mat), ToBullet(pos));
}

class BlenderBulletMotionState : public btMotionState
{
	PHY_IMotionState *m_blenderMotionState;

public:
	BlenderBulletMotionState(PHY_IMotionState *bms)
		:m_blenderMotionState(bms)
	{
	}

	void getWorldTransform(btTransform& worldTrans) const
	{
		const mt::vec3 pos = m_blenderMotionState->GetWorldPosition();
		const mt::mat3 mat = m_blenderMotionState->GetWorldOrientation();
		worldTrans.setOrigin(ToBullet(pos));
		worldTrans.setBasis(ToBullet(mat));
	}

	void setWorldTransform(const btTransform& worldTrans)
	{
		m_blenderMotionState->SetWorldPosition(ToMt(worldTrans.getOrigin()));
		m_blenderMotionState->SetWorldOrientation(ToMt(worldTrans.getBasis()));
		m_blenderMotionState->CalculateWorldTransformations();
	}
};

btRigidBody *CcdPhysicsController::GetRigidBody()
{
	return btRigidBody::upcast(m_object);
}
const btRigidBody *CcdPhysicsController::GetRigidBody() const
{
	return btRigidBody::upcast(m_object);
}

btCollisionObject *CcdPhysicsController::GetCollisionObject()
{
	return m_object;
}
btSoftBody *CcdPhysicsController::GetSoftBody()
{
	return btSoftBody::upcast(m_object);
}
btKinematicCharacterController *CcdPhysicsController::GetCharacterController()
{
	return m_characterController;
}

const std::vector<unsigned int>& CcdPhysicsController::GetSoftBodyIndices() const
{
	return m_softBodyIndices;
}

#include "BulletSoftBody/btSoftBodyHelpers.h"

bool CcdPhysicsController::CreateSoftbody()
{
	int shapeType = m_cci.m_collisionShape ? m_cci.m_collisionShape->getShapeType() : 0;

	//disable soft body until first sneak preview is ready
	if (!m_cci.m_bSoft || !m_cci.m_collisionShape ||
	    ((shapeType != CONVEX_HULL_SHAPE_PROXYTYPE) &&
	     (shapeType != TRIANGLE_MESH_SHAPE_PROXYTYPE) &&
	     (shapeType != SCALED_TRIANGLE_MESH_SHAPE_PROXYTYPE))) {
		return false;
	}

	btSoftBody *psb = nullptr;
	btSoftBodyWorldInfo& worldInfo = m_cci.m_physicsEnv->GetDynamicsWorld()->getWorldInfo();

	if (m_cci.m_collisionShape->getShapeType() == CONVEX_HULL_SHAPE_PROXYTYPE) {
		btConvexHullShape *convexHull = (btConvexHullShape *)m_cci.m_collisionShape;
		{
			int nvertices = convexHull->getNumPoints();
			const btVector3 *vertices = convexHull->getPoints();

			HullDesc hdsc(QF_TRIANGLES, nvertices, vertices);
			HullResult hres;
			HullLibrary hlib;
			hdsc.mMaxVertices = nvertices;
			hlib.CreateConvexHull(hdsc, hres);

			psb = new btSoftBody(&worldInfo, (int)hres.mNumOutputVertices,
			                     &hres.m_OutputVertices[0], 0);
			for (int i = 0; i < (int)hres.mNumFaces; ++i) {
				const unsigned int idx[3] = {hres.m_Indices[i * 3 + 0],
					                         hres.m_Indices[i * 3 + 1],
					                         hres.m_Indices[i * 3 + 2]};
				if (idx[0] < idx[1]) {
					psb->appendLink(idx[0], idx[1]);
				}
				if (idx[1] < idx[2]) {
					psb->appendLink(idx[1], idx[2]);
				}
				if (idx[2] < idx[0]) {
					psb->appendLink(idx[2], idx[0]);
				}
				psb->appendFace(idx[0], idx[1], idx[2]);
			}
			hlib.ReleaseResult(hres);
		}
	}
	else {
		int numtris = 0;
		if (m_cci.m_collisionShape->getShapeType() == SCALED_TRIANGLE_MESH_SHAPE_PROXYTYPE) {
			btScaledBvhTriangleMeshShape *scaledtrimeshshape = (btScaledBvhTriangleMeshShape *)m_cci.m_collisionShape;
			btBvhTriangleMeshShape *trimeshshape = scaledtrimeshshape->getChildShape();

			///only deal with meshes that have 1 sub part/component, for now
			if (trimeshshape->getMeshInterface()->getNumSubParts() == 1) {
				unsigned char *vertexBase;
				btScalar *scaledVertexBase;
				btVector3 localScaling;
				PHY_ScalarType vertexType;
				int numverts;
				int vertexstride;
				unsigned char *indexbase;
				int indexstride;
				PHY_ScalarType indexType;
				trimeshshape->getMeshInterface()->getLockedVertexIndexBase(&vertexBase, numverts, vertexType, vertexstride, &indexbase, indexstride, numtris, indexType);
				localScaling = scaledtrimeshshape->getLocalScaling();
				scaledVertexBase = new btScalar[numverts * 3];
				for (int i = 0; i < numverts * 3; i += 3) {
					scaledVertexBase[i] = ((const btScalar *)vertexBase)[i] * localScaling.getX();
					scaledVertexBase[i + 1] = ((const btScalar *)vertexBase)[i + 1] * localScaling.getY();
					scaledVertexBase[i + 2] = ((const btScalar *)vertexBase)[i + 2] * localScaling.getZ();
				}
				psb = btSoftBodyHelpers::CreateFromTriMesh(worldInfo, scaledVertexBase, (const int *)indexbase, numtris, false);
				delete[] scaledVertexBase;
			}
		}
		else {
			btTriangleMeshShape *trimeshshape = (btTriangleMeshShape *)m_cci.m_collisionShape;
			///only deal with meshes that have 1 sub part/component, for now
			if (trimeshshape->getMeshInterface()->getNumSubParts() == 1) {
				unsigned char *vertexBase;
				PHY_ScalarType vertexType;
				int numverts;
				int vertexstride;
				unsigned char *indexbase;
				int indexstride;
				PHY_ScalarType indexType;
				trimeshshape->getMeshInterface()->getLockedVertexIndexBase(&vertexBase, numverts, vertexType, vertexstride, &indexbase, indexstride, numtris, indexType);

				psb = btSoftBodyHelpers::CreateFromTriMesh(worldInfo, (const btScalar *)vertexBase, (const int *)indexbase, numtris, false);
			}
		}
		// store face tag so that we can find our original face when doing ray casting
		btSoftBody::Face *ft;
		int i;
		for (i = 0, ft = &psb->m_faces[0]; i < numtris; ++i, ++ft) {
			// Hack!! use m_tag to store the face number, normally it is a pointer
			// add 1 to make sure it is never 0
			ft->m_tag = (void *)((uintptr_t)(i + 1));
		}
	}
	if (m_cci.m_margin > 0.0f) {
		psb->getCollisionShape()->setMargin(m_cci.m_margin);
		psb->updateBounds();
	}
	m_object = psb;

	btSoftBody::Material *pm = psb->m_materials[0];
	pm->m_kLST = m_cci.m_soft_linStiff;
	pm->m_kAST = m_cci.m_soft_angStiff;
	pm->m_kVST = m_cci.m_soft_volume;
	psb->m_cfg.collisions = 0;

	if (m_cci.m_soft_collisionflags & CCD_BSB_COL_CL_RS) {
		psb->m_cfg.collisions += btSoftBody::fCollision::CL_RS;
	}
	else {
		psb->m_cfg.collisions += btSoftBody::fCollision::SDF_RS;
	}
	if (m_cci.m_soft_collisionflags & CCD_BSB_COL_CL_SS) {
		psb->m_cfg.collisions += btSoftBody::fCollision::CL_SS;
	}
	else {
		psb->m_cfg.collisions += btSoftBody::fCollision::VF_SS;
	}

	psb->m_cfg.kSRHR_CL = m_cci.m_soft_kSRHR_CL; // Soft vs rigid hardness [0,1] (cluster only)
	psb->m_cfg.kSKHR_CL = m_cci.m_soft_kSKHR_CL; // Soft vs kinetic hardness [0,1] (cluster only)
	psb->m_cfg.kSSHR_CL = m_cci.m_soft_kSSHR_CL; // Soft vs soft hardness [0,1] (cluster only)
	psb->m_cfg.kSR_SPLT_CL = m_cci.m_soft_kSR_SPLT_CL; // Soft vs rigid impulse split [0,1] (cluster only)

	psb->m_cfg.kSK_SPLT_CL = m_cci.m_soft_kSK_SPLT_CL; // Soft vs rigid impulse split [0,1] (cluster only)
	psb->m_cfg.kSS_SPLT_CL = m_cci.m_soft_kSS_SPLT_CL; // Soft vs rigid impulse split [0,1] (cluster only)
	psb->m_cfg.kVCF = m_cci.m_soft_kVCF; // Velocities correction factor (Baumgarte)
	psb->m_cfg.kDP = m_cci.m_soft_kDP; // Damping coefficient [0,1]

	psb->m_cfg.kDG = m_cci.m_soft_kDG; // Drag coefficient [0,+inf]
	psb->m_cfg.kLF = m_cci.m_soft_kLF; // Lift coefficient [0,+inf]
	psb->m_cfg.kPR = m_cci.m_soft_kPR; // Pressure coefficient [-inf,+inf]
	psb->m_cfg.kVC = m_cci.m_soft_kVC; // Volume conversation coefficient [0,+inf]

	psb->m_cfg.kDF = m_cci.m_soft_kDF; // Dynamic friction coefficient [0,1]
	psb->m_cfg.kMT = m_cci.m_soft_kMT; // Pose matching coefficient [0,1]
	psb->m_cfg.kCHR = m_cci.m_soft_kCHR; // Rigid contacts hardness [0,1]
	psb->m_cfg.kKHR = m_cci.m_soft_kKHR; // Kinetic contacts hardness [0,1]

	psb->m_cfg.kSHR = m_cci.m_soft_kSHR; // Soft contacts hardness [0,1]
	psb->m_cfg.kAHR = m_cci.m_soft_kAHR; // Anchors hardness [0,1]

	if (m_cci.m_gamesoftFlag & CCD_BSB_BENDING_CONSTRAINTS) {
		psb->generateBendingConstraints(m_cci.m_softBendingDistance, pm);
	}

	psb->m_cfg.piterations = m_cci.m_soft_piterations;
	psb->m_cfg.viterations = m_cci.m_soft_viterations;
	psb->m_cfg.diterations = m_cci.m_soft_diterations;
	psb->m_cfg.citerations = m_cci.m_soft_citerations;

	if (m_cci.m_gamesoftFlag & CCD_BSB_SHAPE_MATCHING) {
		psb->setPose(false, true);
	}
	else {
		psb->setPose(true, false);
	}

	psb->randomizeConstraints();
	psb->setTotalMass(m_cci.m_mass);

	if (m_cci.m_soft_collisionflags & (CCD_BSB_COL_CL_RS + CCD_BSB_COL_CL_SS)) {
		psb->generateClusters(m_cci.m_soft_numclusteriterations);
	}


	psb->setCollisionFlags(0);

	const unsigned int numVertices = m_shapeInfo->m_vertexRemap.size();
	m_softBodyIndices.resize(numVertices);
	for (unsigned int i = 0; i < numVertices; ++i) {
		const unsigned int index = m_shapeInfo->m_vertexRemap[i];
		if (index == -1) {
			m_softBodyIndices[i] = 0;
			continue;
		}
		const float *co = &m_shapeInfo->m_vertexArray[index * 3];
		m_softBodyIndices[i] = Ccd_FindClosestNode(psb, btVector3(co[0], co[1], co[2]));
	}

	btTransform startTrans;
	m_bulletMotionState->getWorldTransform(startTrans);

	m_MotionState->SetWorldPosition(ToMt(startTrans.getOrigin()));
	m_MotionState->SetWorldOrientation(mt::mat3::Identity());

	psb->transform(startTrans);

	m_object->setCollisionFlags(m_object->getCollisionFlags() | m_cci.m_collisionFlags);
	if (m_cci.m_do_anisotropic) {
		m_object->setAnisotropicFriction(m_cci.m_anisotropicFriction);
	}

	return true;
}

bool CcdPhysicsController::CreateCharacterController()
{
	if (!m_cci.m_bCharacter) {
		return false;
	}

	m_object = new btPairCachingGhostObject();
	m_object->setCollisionShape(m_collisionShape);
	m_object->setCollisionFlags(btCollisionObject::CF_CHARACTER_OBJECT);

	btTransform trans;
	m_bulletMotionState->getWorldTransform(trans);
	m_object->setWorldTransform(trans);

	m_characterController = new CcdCharacter(this, m_bulletMotionState, (btPairCachingGhostObject *)m_object,
	                                         (btConvexShape *)m_collisionShape, m_cci.m_stepHeight);

	m_characterController->setJumpSpeed(m_cci.m_jumpSpeed);
	m_characterController->setFallSpeed(m_cci.m_fallSpeed);
	m_characterController->setMaxJumps(m_cci.m_maxJumps);
	m_characterController->setMaxSlope(m_cci.m_maxSlope);

	return true;
}

void CcdPhysicsController::CreateRigidbody()
{
	//btTransform trans = GetTransformFromMotionState(m_MotionState);
	m_bulletMotionState = new BlenderBulletMotionState(m_MotionState);

	///either create a btCollisionObject, btRigidBody or btSoftBody
	if (CreateSoftbody() || CreateCharacterController()) {
		// soft body created, done
		return;
	}

	//create a rgid collision object
	btRigidBody::btRigidBodyConstructionInfo rbci(m_cci.m_mass, m_bulletMotionState, m_collisionShape, m_cci.m_localInertiaTensor *m_cci.m_inertiaFactor);
	rbci.m_linearDamping = m_cci.m_linearDamping;
	rbci.m_angularDamping = m_cci.m_angularDamping;
	rbci.m_friction = m_cci.m_friction;
	rbci.m_rollingFriction = m_cci.m_rollingFriction;
	rbci.m_restitution = m_cci.m_restitution;
	m_object = new btRigidBody(rbci);

	//
	// init the rigidbody properly
	//

	//setMassProps this also sets collisionFlags
	//convert collision flags!
	//special case: a near/radar sensor controller should not be defined static or it will
	//generate loads of static-static collision messages on the console
	if (m_cci.m_bSensor) {
		// reset the flags that have been set so far
		m_object->setCollisionFlags(0);
		// sensor must never go to sleep: they need to detect continously
		m_object->setActivationState(DISABLE_DEACTIVATION);
	}
	m_object->setCollisionFlags(m_object->getCollisionFlags() | m_cci.m_collisionFlags);
	btRigidBody *body = GetRigidBody();

	if (body) {
		body->setGravity(m_cci.m_gravity);
		body->setDamping(m_cci.m_linearDamping, m_cci.m_angularDamping);

		if (!m_cci.m_bRigid) {
			body->setAngularFactor(0.0f);
		}
		// use bullet's default contact processing theshold, blender's old default of 1 is too small here.
		// if there's really a need to change this, it should be exposed in the ui first.
//		body->setContactProcessingThreshold(m_cci.m_contactProcessingThreshold);
		body->setSleepingThresholds(gLinearSleepingTreshold, gAngularSleepingTreshold);

	}
	if (m_object && m_cci.m_do_anisotropic) {
		m_object->setAnisotropicFriction(m_cci.m_anisotropicFriction);
	}
}

mt::vec3 CcdPhysicsController::GetGravity()
{
	btRigidBody *body = GetRigidBody();
	if (body) {
		return ToMt(body->getGravity());
	}
	return mt::zero3;
}

void CcdPhysicsController::SetGravity(const mt::vec3 &gravity)
{
	btRigidBody *body = GetRigidBody();
	if (body) {
		body->setGravity(ToBullet(gravity));
	}
}

static void DeleteBulletShape(btCollisionShape *shape, bool free)
{
	if (shape->getShapeType() == SCALED_TRIANGLE_MESH_SHAPE_PROXYTYPE) {
		/* If we use Bullet scaled shape (btScaledBvhTriangleMeshShape) we have to
		 * free the child of the unscaled shape (btTriangleMeshShape) here.
		 */
		btTriangleMeshShape *meshShape = ((btScaledBvhTriangleMeshShape *)shape)->getChildShape();
		if (meshShape) {
			delete meshShape;
		}
	}
	if (free) {
		delete shape;
	}
}

bool CcdPhysicsController::DeleteControllerShape()
{
	if (m_collisionShape) {
		// collision shape is always unique to the controller, can delete it here
		if (m_collisionShape->isCompound()) {
			// bullet does not delete the child shape, must do it here
			btCompoundShape *compoundShape = (btCompoundShape *)m_collisionShape;
			int numChild = compoundShape->getNumChildShapes();
			for (int i = numChild - 1; i >= 0; i--) {
				btCollisionShape *childShape = compoundShape->getChildShape(i);
				DeleteBulletShape(childShape, true);
			}
		}
		DeleteBulletShape(m_collisionShape, true);

		return true;
	}

	return false;
}

bool CcdPhysicsController::ReplaceControllerShape(btCollisionShape *newShape)
{
	if (m_collisionShape) {
		DeleteControllerShape();
	}

	// If newShape is nullptr it means to create a new Bullet shape.
	if (!newShape) {
		newShape = m_shapeInfo->CreateBulletShape(m_cci.m_margin, m_cci.m_bGimpact, !m_cci.m_bSoft);
	}

	m_object->setCollisionShape(newShape);
	m_collisionShape = newShape;
	m_cci.m_collisionShape = newShape;

	btSoftBody *softBody = GetSoftBody();
	if (softBody) {
		btSoftRigidDynamicsWorldMt *world = m_cci.m_physicsEnv->GetDynamicsWorld();
		// remove the old softBody
		world->removeSoftBody(softBody);

		// soft body must be recreated
		delete m_object;
		m_object = nullptr;
		// force complete reinitialization
		m_softBodyTransformInitialized = false;

		CreateSoftbody();
		BLI_assert(m_object);

		btSoftBody *newSoftBody = GetSoftBody();
		// set the user
		newSoftBody->setUserPointer(this);
		// add the new softbody
		world->addSoftBody(newSoftBody);
	}

	return true;
}

CcdPhysicsController::~CcdPhysicsController()
{
	//will be reference counted, due to sharing
	if (m_cci.m_physicsEnv) {
		m_cci.m_physicsEnv->RemoveCcdPhysicsController(this, true);
	}

	if (m_MotionState) {
		delete m_MotionState;
	}
	if (m_bulletMotionState) {
		delete m_bulletMotionState;
	}
	if (m_characterController) {
		delete m_characterController;
	}
	delete m_object;

	DeleteControllerShape();

	if (m_shapeInfo) {
		m_shapeInfo->Release();
	}
}

void CcdPhysicsController::SimulationTick(float timestep)
{
	btRigidBody *body = GetRigidBody();
	if (!body || body->isStaticObject()) {
		return;
	}

	// Clamp linear velocity
	if (m_cci.m_clamp_vel_max > 0.0f || m_cci.m_clamp_vel_min > 0.0f) {
		const btVector3 &linvel = body->getLinearVelocity();
		btScalar len = linvel.length();

		if (m_cci.m_clamp_vel_max > 0.0f && len > m_cci.m_clamp_vel_max) {
			body->setLinearVelocity(linvel * (m_cci.m_clamp_vel_max / len));
		}
		else if (m_cci.m_clamp_vel_min > 0.0f && !btFuzzyZero(len) && len < m_cci.m_clamp_vel_min) {
			body->setLinearVelocity(linvel * (m_cci.m_clamp_vel_min / len));
		}
	}

	// Clamp angular velocity
	if (m_cci.m_clamp_angvel_max > 0.0f || m_cci.m_clamp_angvel_min > 0.0f) {
		const btVector3 &angvel = body->getAngularVelocity();
		btScalar len = angvel.length();

		if (m_cci.m_clamp_angvel_max > 0.0f && len > m_cci.m_clamp_angvel_max) {
			body->setAngularVelocity(angvel * (m_cci.m_clamp_angvel_max / len));
		}
		else if (m_cci.m_clamp_angvel_min > 0.0f && !btFuzzyZero(len) && len < m_cci.m_clamp_angvel_min) {
			body->setAngularVelocity(angvel * (m_cci.m_clamp_angvel_min / len));
		}
	}
}

/**
 * SynchronizeMotionStates ynchronizes dynas, kinematic and deformable entities (and do 'late binding')
 */
bool CcdPhysicsController::SynchronizeMotionStates(float time)
{
	//sync non-static to motionstate, and static from motionstate (todo: add kinematic etc.)

	btSoftBody *sb = GetSoftBody();
	if (sb) {
		if (sb->m_pose.m_bframe) {
			btVector3 worldPos = sb->m_pose.m_com;
			btQuaternion worldquat;
			btMatrix3x3 trs = sb->m_pose.m_rot * sb->m_pose.m_scl;
			trs.getRotation(worldquat);
			m_MotionState->SetWorldPosition(ToMt(worldPos));
			m_MotionState->SetWorldOrientation(ToMt(worldquat));
		}
		else {
			btVector3 aabbMin, aabbMax;
			sb->getAabb(aabbMin, aabbMax);
			btVector3 worldPos  = (aabbMax + aabbMin) * 0.5f;
			m_MotionState->SetWorldPosition(ToMt(worldPos));
		}
		m_MotionState->CalculateWorldTransformations();
		return true;
	}

	btRigidBody *body = GetRigidBody();

	if (body && !body->isStaticObject()) {
		const btTransform& xform = body->getCenterOfMassTransform();
		const btMatrix3x3& worldOri = xform.getBasis();
		const btVector3& worldPos = xform.getOrigin();
		m_MotionState->SetWorldOrientation(ToMt(worldOri));
		m_MotionState->SetWorldPosition(ToMt(worldPos));
		m_MotionState->CalculateWorldTransformations();
	}

	const mt::vec3& scale = m_MotionState->GetWorldScaling();
	GetCollisionShape()->setLocalScaling(ToBullet(scale));

	return true;
}

/**
 * WriteMotionStateToDynamics synchronizes dynas, kinematic and deformable entities (and do 'late binding')
 */

void CcdPhysicsController::WriteMotionStateToDynamics(bool nondynaonly)
{
	btTransform xform = CcdPhysicsController::GetTransformFromMotionState(m_MotionState);
	SetCenterOfMassTransform(xform);
}

void CcdPhysicsController::WriteDynamicsToMotionState()
{
}
// controller replication
void CcdPhysicsController::PostProcessReplica(class PHY_IMotionState *motionstate, class PHY_IPhysicsController *parentctrl)
{
	SetParentRoot((CcdPhysicsController *)parentctrl);
	m_softBodyTransformInitialized = false;
	m_MotionState = motionstate;
	m_registerCount = 0;
	m_collisionShape = nullptr;

	// Clear all old constraints.
	m_ccdConstraintRefs.clear();

	// always create a new shape to avoid scaling bug
	if (m_shapeInfo) {
		m_shapeInfo->AddRef();
		m_collisionShape = m_shapeInfo->CreateBulletShape(m_cci.m_margin, m_cci.m_bGimpact, !m_cci.m_bSoft);

		if (m_collisionShape) {
			// new shape has no scaling, apply initial scaling
			//m_collisionShape->setMargin(m_cci.m_margin);
			m_collisionShape->setLocalScaling(m_cci.m_scaling);

			if (m_cci.m_mass) {
				m_collisionShape->calculateLocalInertia(m_cci.m_mass, m_cci.m_localInertiaTensor);
			}
		}
	}

	// load some characterists that are not
	btRigidBody *oldbody = GetRigidBody();
	m_object = nullptr;
	CreateRigidbody();
	btRigidBody *body = GetRigidBody();
	if (body) {
		if (m_cci.m_mass) {
			body->setMassProps(m_cci.m_mass, m_cci.m_localInertiaTensor * m_cci.m_inertiaFactor);
		}

		if (oldbody) {
			body->setLinearFactor(oldbody->getLinearFactor());
			body->setAngularFactor(oldbody->getAngularFactor());
			if (oldbody->getActivationState() == DISABLE_DEACTIVATION) {
				body->setActivationState(DISABLE_DEACTIVATION);
			}
		}
	}
	// sensor object are added when needed
	if (!m_cci.m_bSensor) {
		m_cci.m_physicsEnv->AddCcdPhysicsController(this);
	}
}

void CcdPhysicsController::SetPhysicsEnvironment(class PHY_IPhysicsEnvironment *env)
{
	// can safely assume CCD environment
	CcdPhysicsEnvironment *physicsEnv = static_cast<CcdPhysicsEnvironment *>(env);

	if (m_cci.m_physicsEnv != physicsEnv) {
		// since the environment is changing, we must also move the controler to the
		// new environment. Note that we don't handle sensor explicitly: this
		// function can be called on sensor but only when they are not registered
		if (m_cci.m_physicsEnv->RemoveCcdPhysicsController(this, true)) {
			physicsEnv->AddCcdPhysicsController(this);

			// Set the object to be active so it can at least by evaluated once.
			// This fixes issues with static objects not having their physics meshes
			// in the right spot when lib loading.
			m_object->setActivationState(ACTIVE_TAG);
		}
		m_cci.m_physicsEnv = physicsEnv;
	}
}

void CcdPhysicsController::SetCenterOfMassTransform(btTransform& xform)
{
	btRigidBody *body = GetRigidBody();
	if (body) {
		body->setCenterOfMassTransform(xform);
	}
	else {
		//either collision object or soft body?
		if (GetSoftBody()) {
		}
		else {
			if (m_object->isStaticOrKinematicObject()) {
				m_object->setInterpolationWorldTransform(m_object->getWorldTransform());
			}
			else {
				m_object->setInterpolationWorldTransform(xform);
			}
			m_object->setWorldTransform(xform);
		}
	}
}

// kinematic methods
void CcdPhysicsController::RelativeTranslate(const mt::vec3& dlocin, bool local)
{
	if (m_object) {
		m_object->activate(true);
		if (m_object->isStaticObject()) {
			if (!m_cci.m_bSensor) {
				m_object->setCollisionFlags(m_object->getCollisionFlags() | btCollisionObject::CF_KINEMATIC_OBJECT);
			}
			// kinematic object should not set the transform, it disturbs the velocity interpolation
			return;
		}

		btVector3 dloc = ToBullet(dlocin);
		btTransform xform = m_object->getWorldTransform();

		if (local) {
			dloc = xform.getBasis() * dloc;
		}

		xform.setOrigin(xform.getOrigin() + dloc);
		SetCenterOfMassTransform(xform);
	}
}

void CcdPhysicsController::RelativeRotate(const mt::mat3& rotval, bool local)
{
	if (m_object) {
		m_object->activate(true);
		if (m_object->isStaticObject()) {
			if (!m_cci.m_bSensor) {
				m_object->setCollisionFlags(m_object->getCollisionFlags() | btCollisionObject::CF_KINEMATIC_OBJECT);
			}
			// kinematic object should not set the transform, it disturbs the velocity interpolation
			return;
		}

		btMatrix3x3 drotmat = ToBullet(rotval);
		btMatrix3x3 currentOrn;
		GetWorldOrientation(currentOrn);

		btTransform xform = m_object->getWorldTransform();

		xform.setBasis(xform.getBasis() * (local ?
		                                   drotmat : (currentOrn.inverse() * drotmat * currentOrn)));

		SetCenterOfMassTransform(xform);
	}
}

void CcdPhysicsController::GetWorldOrientation(btMatrix3x3& mat)
{
	const mt::mat3 ori = m_MotionState->GetWorldOrientation();
	mat = ToBullet(ori);
}

mt::mat3 CcdPhysicsController::GetOrientation()
{
	const btMatrix3x3 orn = m_object->getWorldTransform().getBasis();
	return ToMt(orn);
}

void CcdPhysicsController::SetOrientation(const mt::mat3& orn)
{
	SetWorldOrientation(ToBullet(orn));
}

void CcdPhysicsController::SetWorldOrientation(const btMatrix3x3& orn)
{
	if (m_object) {
		m_object->activate(true);
		if (m_object->isStaticObject() && !m_cci.m_bSensor) {
			m_object->setCollisionFlags(m_object->getCollisionFlags() | btCollisionObject::CF_KINEMATIC_OBJECT);
		}
		btTransform xform  = m_object->getWorldTransform();
		xform.setBasis(orn);
		SetCenterOfMassTransform(xform);

		//only once!
		if (!m_softBodyTransformInitialized && GetSoftBody()) {
			m_softbodyStartTrans.setBasis(orn);
			xform.setOrigin(m_softbodyStartTrans.getOrigin());
			GetSoftBody()->transform(xform);
			m_softBodyTransformInitialized = true;
		}
	}
}

void CcdPhysicsController::SetPosition(const mt::vec3& pos)
{
	if (m_object) {
		m_object->activate(true);
		if (m_object->isStaticObject()) {
			if (!m_cci.m_bSensor) {
				m_object->setCollisionFlags(m_object->getCollisionFlags() | btCollisionObject::CF_KINEMATIC_OBJECT);
			}
			// kinematic object should not set the transform, it disturbs the velocity interpolation
			return;
		}

		btTransform xform  = m_object->getWorldTransform();
		xform.setOrigin(ToBullet(pos));
		SetCenterOfMassTransform(xform);
		if (!m_softBodyTransformInitialized) {
			m_softbodyStartTrans.setOrigin(xform.getOrigin());
		}
	}
}

void CcdPhysicsController::ForceWorldTransform(const btMatrix3x3& mat, const btVector3& pos)
{
	if (m_object) {
		btTransform& xform = m_object->getWorldTransform();
		xform.setBasis(mat);
		xform.setOrigin(pos);
	}
}

void CcdPhysicsController::RefreshCollisions()
{
	// the object is in an inactive layer so it's useless to update it and can cause problems
	if (IsPhysicsSuspended()) {
		return;
	}

	btDynamicsWorld *dw = m_cci.m_physicsEnv->GetDynamicsWorld();
	btBroadphaseProxy *proxy = m_object->getBroadphaseHandle();
	btDispatcher *dispatcher = dw->getDispatcher();
	btOverlappingPairCache *pairCache = dw->getPairCache();

	CleanPairCallback cleanPairs(proxy, pairCache, dispatcher);
	pairCache->processAllOverlappingPairs(&cleanPairs, dispatcher);

	// Forcibly recreate the physics object
	btBroadphaseProxy *handle = m_object->getBroadphaseHandle();
	m_cci.m_physicsEnv->UpdateCcdPhysicsController(this, GetMass(), m_object->getCollisionFlags(), handle->m_collisionFilterGroup, handle->m_collisionFilterMask);
}

void CcdPhysicsController::SuspendPhysics(bool freeConstraints)
{
	m_cci.m_physicsEnv->RemoveCcdPhysicsController(this, freeConstraints);
}

void CcdPhysicsController::RestorePhysics()
{
	m_cci.m_physicsEnv->AddCcdPhysicsController(this);
}

void CcdPhysicsController::SuspendDynamics(bool ghost)
{
	btRigidBody *body = GetRigidBody();
	if (body && !m_suspended && !m_cci.m_bSensor && !IsPhysicsSuspended()) {
		btBroadphaseProxy *handle = body->getBroadphaseHandle();

		m_savedCollisionFlags = body->getCollisionFlags();
		m_savedMass = GetMass();
		m_savedDyna = m_cci.m_bDyna;
		m_savedCollisionFilterGroup = handle->m_collisionFilterGroup;
		m_savedCollisionFilterMask = handle->m_collisionFilterMask;
		m_suspended = true;
		m_cci.m_physicsEnv->UpdateCcdPhysicsController(this,
		                                                    0.0f,
		                                                    btCollisionObject::CF_STATIC_OBJECT | ((ghost) ? btCollisionObject::CF_NO_CONTACT_RESPONSE : (m_savedCollisionFlags & btCollisionObject::CF_NO_CONTACT_RESPONSE)),
		                                                    btBroadphaseProxy::StaticFilter,
		                                                    btBroadphaseProxy::AllFilter ^ btBroadphaseProxy::StaticFilter);
		m_cci.m_bDyna = false;
	}
}

void CcdPhysicsController::RestoreDynamics()
{
	btRigidBody *body = GetRigidBody();
	if (body && m_suspended && !IsPhysicsSuspended()) {
		// before make sure any position change that was done in this logic frame are accounted for
		SetTransform();
		m_cci.m_physicsEnv->UpdateCcdPhysicsController(this,
		                                                    m_savedMass,
		                                                    m_savedCollisionFlags,
		                                                    m_savedCollisionFilterGroup,
		                                                    m_savedCollisionFilterMask);
		body->activate();
		m_cci.m_bDyna = m_savedDyna;
		m_suspended = false;
	}
}

mt::vec3 CcdPhysicsController::GetPosition() const
{
	return ToMt(m_object->getWorldTransform().getOrigin());
}

void CcdPhysicsController::SetScaling(const mt::vec3& scale)
{
	if (!btFuzzyZero(m_cci.m_scaling.x() - scale.x) ||
	    !btFuzzyZero(m_cci.m_scaling.y() - scale.y) ||
	    !btFuzzyZero(m_cci.m_scaling.z() - scale.z)) {
		m_cci.m_scaling = ToBullet(scale);

		if (m_object && m_object->getCollisionShape()) {
			m_object->activate(true); // without this, sleeping objects scale wont be applied in bullet if python changes the scale - Campbell.
			m_object->getCollisionShape()->setLocalScaling(m_cci.m_scaling);

			btRigidBody *body = GetRigidBody();
			if (body && m_cci.m_mass) {
				body->getCollisionShape()->calculateLocalInertia(m_cci.m_mass, m_cci.m_localInertiaTensor);
				body->setMassProps(m_cci.m_mass, m_cci.m_localInertiaTensor * m_cci.m_inertiaFactor);
			}
		}
	}
}

void CcdPhysicsController::SetTransform()
{
	const mt::vec3 pos = m_MotionState->GetWorldPosition();
	const mt::mat3 rot = m_MotionState->GetWorldOrientation();
	ForceWorldTransform(ToBullet(rot), ToBullet(pos));

	if (!IsDynamic() && !GetConstructionInfo().m_bSensor && !m_characterController) {
		btCollisionObject *object = GetRigidBody();
		object->setActivationState(ACTIVE_TAG);
		object->setCollisionFlags(object->getCollisionFlags() | btCollisionObject::CF_KINEMATIC_OBJECT);
	}
}

float CcdPhysicsController::GetMass()
{
	if (GetSoftBody()) {
		return GetSoftBody()->getTotalMass();
	}

	float invmass = 0.0f;
	if (GetRigidBody()) {
		invmass = GetRigidBody()->getInvMass();
	}
	if (invmass) {
		return 1.0f / invmass;
	}
	return 0.0f;
}

void CcdPhysicsController::SetMass(float newmass)
{
	btRigidBody *body = GetRigidBody();
	if (body && !m_suspended && !IsPhysicsSuspended() && (!mt::FuzzyZero(newmass) && !mt::FuzzyZero(GetMass()))) {
		btBroadphaseProxy *handle = body->getBroadphaseHandle();
		m_cci.m_physicsEnv->UpdateCcdPhysicsController(this,
		                                                    newmass,
		                                                    body->getCollisionFlags(),
		                                                    handle->m_collisionFilterGroup,
		                                                    handle->m_collisionFilterMask);
	}
}

float CcdPhysicsController::GetInertiaFactor() const
{
	return m_cci.m_inertiaFactor;
}

// physics methods
void CcdPhysicsController::ApplyTorque(const mt::vec3&  torquein, bool local)
{
	btVector3 torque = ToBullet(torquein);
	btTransform xform = m_object->getWorldTransform();


	if (m_object && torque.length2() > (SIMD_EPSILON * SIMD_EPSILON)) {
		btRigidBody *body = GetRigidBody();
		m_object->activate();
		if (m_object->isStaticObject()) {
			if (!m_cci.m_bSensor) {
				m_object->setCollisionFlags(m_object->getCollisionFlags() | btCollisionObject::CF_KINEMATIC_OBJECT);
			}
			return;
		}
		if (local) {
			torque  = xform.getBasis() * torque;
		}
		if (body) {
			if  (m_cci.m_bRigid) {
				body->applyTorque(torque);
			}
			else {
				//workaround for incompatibility between 'DYNAMIC' game object, and angular factor
				//a DYNAMIC object has some inconsistency: it has no angular effect due to collisions, but still has torque
				const btVector3 angFac = body->getAngularFactor();
				btVector3 tmpFac(1.0f, 1.0f, 1.0f);
				body->setAngularFactor(tmpFac);
				body->applyTorque(torque);
				body->setAngularFactor(angFac);
			}
		}
	}
}

void CcdPhysicsController::ApplyForce(const mt::vec3& forcein, bool local)
{
	btVector3 force = ToBullet(forcein);

	if (m_object && force.length2() > (SIMD_EPSILON * SIMD_EPSILON)) {
		m_object->activate();
		if (m_object->isStaticObject()) {
			if (!m_cci.m_bSensor) {
				m_object->setCollisionFlags(m_object->getCollisionFlags() | btCollisionObject::CF_KINEMATIC_OBJECT);
			}
			return;
		}
		btTransform xform = m_object->getWorldTransform();

		if (local) {
			force = xform.getBasis() * force;
		}
		btRigidBody *body = GetRigidBody();
		if (body) {
			body->applyCentralForce(force);
		}
		btSoftBody *soft = GetSoftBody();
		if (soft) {
			// the force is applied on each node, must reduce it in the same extend
			if (soft->m_nodes.size() > 0) {
				force /= soft->m_nodes.size();
			}
			soft->addForce(force);
		}
	}
}
void CcdPhysicsController::SetAngularVelocity(const mt::vec3& ang_vel, bool local)
{
	btVector3 angvel = ToBullet(ang_vel);

	/* Refuse tiny tiny velocities, as they might cause instabilities. */
	float vel_squared = angvel.length2();
	if (vel_squared > 0.0f && vel_squared <= (SIMD_EPSILON * SIMD_EPSILON)) {
		angvel = btVector3(0.0f, 0.0f, 0.0f);
	}

	if (m_object) {
		m_object->activate(true);
		if (m_object->isStaticObject()) {
			if (!m_cci.m_bSensor) {
				m_object->setCollisionFlags(m_object->getCollisionFlags() | btCollisionObject::CF_KINEMATIC_OBJECT);
			}
			return;
		}
		btTransform xform = m_object->getWorldTransform();
		if (local) {
			angvel = xform.getBasis() * angvel;
		}
		btRigidBody *body = GetRigidBody();
		if (body) {
			body->setAngularVelocity(angvel);
		}
	}
}
void CcdPhysicsController::SetLinearVelocity(const mt::vec3& lin_vel, bool local)
{
	btVector3 linVel = ToBullet(lin_vel);

	/* Refuse tiny tiny velocities, as they might cause instabilities. */
	const float vel_squared = linVel.length2();
	if (vel_squared > 0.0f && vel_squared <= (SIMD_EPSILON * SIMD_EPSILON)) {
		linVel = btVector3(0.0f, 0.0f, 0.0f);
	}

	if (m_object) {
		m_object->activate(true);
		if (m_object->isStaticObject()) {
			if (!m_cci.m_bSensor) {
				m_object->setCollisionFlags(m_object->getCollisionFlags() | btCollisionObject::CF_KINEMATIC_OBJECT);
			}
			return;
		}

		btSoftBody *soft = GetSoftBody();
		if (soft) {
			if (local) {
				linVel = m_softbodyStartTrans.getBasis() * linVel;
			}
			soft->setVelocity(linVel);
		}
		else {
			btTransform xform = m_object->getWorldTransform();
			if (local) {
				linVel  = xform.getBasis() * linVel;
			}
			btRigidBody *body = GetRigidBody();
			if (body) {
				body->setLinearVelocity(linVel);
			}
		}
	}
}
void CcdPhysicsController::ApplyImpulse(const mt::vec3& attach, const mt::vec3& impulsein, bool local)
{
	btVector3 impulse = ToBullet(impulsein);

	if (m_object && impulse.length2() > (SIMD_EPSILON * SIMD_EPSILON)) {
		m_object->activate();
		if (m_object->isStaticObject()) {
			if (!m_cci.m_bSensor) {
				m_object->setCollisionFlags(m_object->getCollisionFlags() | btCollisionObject::CF_KINEMATIC_OBJECT);
			}
			return;
		}

		const btTransform xform = m_object->getWorldTransform();
		btVector3 pos;

		if (local) {
			pos = ToBullet(attach);
			impulse = xform.getBasis() * impulse;
		}
		else {
			/* If the point of impulse application is not equal to the object position
			 * then an angular momentum is generated in the object*/
			pos = ToBullet(attach) - xform.getOrigin();
		}

		btRigidBody *body = GetRigidBody();
		if (body) {
			body->applyImpulse(impulse, pos);
		}
	}

}

void CcdPhysicsController::Jump()
{
	if (m_object && m_characterController) {
		m_characterController->jump();
	}
}

void CcdPhysicsController::SetActive(bool active)
{
}

unsigned short CcdPhysicsController::GetCollisionGroup() const
{
	return m_cci.m_collisionGroup;
}

unsigned short CcdPhysicsController::GetCollisionMask() const
{
	return m_cci.m_collisionMask;
}

void CcdPhysicsController::SetCollisionGroup(unsigned short group)
{
	m_cci.m_collisionGroup = group;
}

void CcdPhysicsController::SetCollisionMask(unsigned short mask)
{
	m_cci.m_collisionMask = mask;
}

float CcdPhysicsController::GetLinearDamping() const
{
	const btRigidBody *body = GetRigidBody();
	if (body) {
		return body->getLinearDamping();
	}
	return 0.0f;
}

float CcdPhysicsController::GetAngularDamping() const
{
	const btRigidBody *body = GetRigidBody();
	if (body) {
		return body->getAngularDamping();
	}
	return 0.0f;
}

void CcdPhysicsController::SetLinearDamping(float damping)
{
	SetDamping(damping, GetAngularDamping());
}

void CcdPhysicsController::SetAngularDamping(float damping)
{
	SetDamping(GetLinearDamping(), damping);
}

void CcdPhysicsController::SetDamping(float linear, float angular)
{
	btRigidBody *body = GetRigidBody();
	if (!body) {
		return;
	}

	body->setDamping(linear, angular);
}

// reading out information from physics
mt::vec3 CcdPhysicsController::GetLinearVelocity()
{
	btRigidBody *body = GetRigidBody();
	if (body) {
		const btVector3& linvel = body->getLinearVelocity();
		return ToMt(linvel);
	}

	return mt::zero3;
}

mt::vec3 CcdPhysicsController::GetAngularVelocity()
{
	btRigidBody *body = GetRigidBody();
	if (body) {
		const btVector3& angvel = body->getAngularVelocity();
		return ToMt(angvel);
	}

	return mt::zero3;
}

mt::vec3 CcdPhysicsController::GetVelocity(const mt::vec3 &posin)
{
	btRigidBody *body = GetRigidBody();
	if (body) {
		btVector3 linvel = body->getVelocityInLocalPoint(ToBullet(posin));
		return ToMt(linvel);
	}

	return mt::zero3;
}

mt::vec3 CcdPhysicsController::GetLocalInertia()
{
	btRigidBody *body = GetRigidBody();
	mt::vec3 inertia = mt::zero3;
	if (body) {
		const btVector3 inv_inertia = body->getInvInertiaDiagLocal();
		if (!btFuzzyZero(inv_inertia.getX()) &&
		    !btFuzzyZero(inv_inertia.getY()) &&
		    !btFuzzyZero(inv_inertia.getZ())) {
			inertia = mt::vec3(1.0f / inv_inertia.getX(), 1.0f / inv_inertia.getY(), 1.0f / inv_inertia.getZ());
		}
	}
	return inertia;
}

// dyna's that are rigidbody are free in orientation, dyna's with non-rigidbody are restricted
void CcdPhysicsController::SetRigidBody(bool rigid)
{
	btRigidBody *body = GetRigidBody();
	if (body) {
		m_cci.m_bRigid = rigid;
		if (!rigid) {
			body->setAngularFactor(0.0f);
			body->setAngularVelocity(btVector3(0.0f, 0.0f, 0.0f));
		}
		else {
			body->setAngularFactor(m_cci.m_angularFactor);
		}
	}
}

// clientinfo for raycasts for example
void *CcdPhysicsController::GetNewClientInfo()
{
	return m_newClientInfo;
}

void CcdPhysicsController::SetNewClientInfo(void *clientinfo)
{
	m_newClientInfo = clientinfo;

	if (m_cci.m_bSensor) {
		// use a different callback function for sensor object,
		// bullet will not synchronize, we must do it explicitly
		SG_Callbacks& callbacks = KX_GameObject::GetClientObject((KX_ClientObjectInfo *)clientinfo)->GetNode()->GetCallBackFunctions();
		callbacks.m_updatefunc = KX_GameObject::SynchronizeTransformFunc;
	}
}

void CcdPhysicsController::UpdateDeactivation(float timeStep)
{
	btRigidBody *body = GetRigidBody();
	if (body) {
		body->updateDeactivation(timeStep);
	}
}

bool CcdPhysicsController::WantsSleeping()
{
	btRigidBody *body = GetRigidBody();
	if (body) {
		return body->wantsSleeping();
	}
	//check it out
	return true;
}
/* This function dynamically adds the collision shape of another controller to
 * the current controller shape provided it is a compound shape.
 * The idea is that dynamic parenting on a compound object will dynamically extend the shape
 */
void CcdPhysicsController::AddCompoundChild(PHY_IPhysicsController *child)
{
	if (child == nullptr || !IsCompound()) {
		return;
	}
	// other controller must be a bullet controller too
	// verify that body and shape exist and match
	CcdPhysicsController *childCtrl = static_cast<CcdPhysicsController *>(child);
	btRigidBody *rootBody = GetRigidBody();
	btRigidBody *childBody = childCtrl->GetRigidBody();
	if (!rootBody || !childBody) {
		return;
	}
	const btCollisionShape *rootShape = rootBody->getCollisionShape();
	const btCollisionShape *childShape = childBody->getCollisionShape();
	if (!rootShape ||
	    !childShape ||
	    rootShape->getShapeType() != COMPOUND_SHAPE_PROXYTYPE) {
		return;
	}
	btCompoundShape *compoundShape = (btCompoundShape *)rootShape;

	// compute relative transformation between parent and child
	btTransform rootTrans;
	btTransform childTrans;
	rootBody->getMotionState()->getWorldTransform(rootTrans);
	childBody->getMotionState()->getWorldTransform(childTrans);
	btVector3 rootScale = rootShape->getLocalScaling();
	rootScale[0] = 1.0 / rootScale[0];
	rootScale[1] = 1.0 / rootScale[1];
	rootScale[2] = 1.0 / rootScale[2];
	// relative scale = child_scale/parent_scale
	const btVector3 relativeScale = childShape->getLocalScaling() * rootScale;
	const btMatrix3x3 rootRotInverse = rootTrans.getBasis().transpose();
	// relative pos = parent_rot^-1 * ((parent_pos-child_pos)/parent_scale)
	const btVector3 relativePos = rootRotInverse * ((childTrans.getOrigin() - rootTrans.getOrigin()) * rootScale);
	// relative rot = parent_rot^-1 * child_rot
	const btMatrix3x3 relativeRot = rootRotInverse * childTrans.getBasis();

	// create a proxy shape info to store the transformation
	CcdShapeConstructionInfo *proxyShapeInfo = new CcdShapeConstructionInfo();
	// store the transformation to this object shapeinfo
	proxyShapeInfo->m_childTrans.setOrigin(relativePos);
	proxyShapeInfo->m_childTrans.setBasis(relativeRot);
	proxyShapeInfo->m_childScale = relativeScale;
	// we will need this to make sure that we remove the right proxy later when unparenting
	proxyShapeInfo->m_userData = childCtrl;
	proxyShapeInfo->SetProxy(childCtrl->GetShapeInfo()->AddRef());
	// add to parent compound shapeinfo (increments ref count)
	GetShapeInfo()->AddShape(proxyShapeInfo);
	// create new bullet collision shape from the object shapeinfo and set scaling
	btCollisionShape *newChildShape = proxyShapeInfo->CreateBulletShape(childCtrl->GetMargin(), childCtrl->GetConstructionInfo().m_bGimpact, true);
	newChildShape->setLocalScaling(relativeScale);
	// add bullet collision shape to parent compound collision shape
	compoundShape->addChildShape(proxyShapeInfo->m_childTrans, newChildShape);
	// proxyShapeInfo is not needed anymore, release it
	proxyShapeInfo->Release();
	// remember we created this shape
	childCtrl->m_bulletChildShape = newChildShape;

	// Recalculate inertia for object owning compound shape.
	if (!rootBody->isStaticOrKinematicObject()) {
		btVector3 localInertia;
		const float mass = 1.0f / rootBody->getInvMass();
		compoundShape->calculateLocalInertia(mass, localInertia);
		rootBody->setMassProps(mass, localInertia * m_cci.m_inertiaFactor);
	}
	// must update the broadphase cache,
	m_cci.m_physicsEnv->RefreshCcdPhysicsController(this);
	// remove the children
	m_cci.m_physicsEnv->RemoveCcdPhysicsController(childCtrl, true);
}

/* Reverse function of the above, it will remove a shape from a compound shape
 * provided that the former was added to the later using  AddCompoundChild()
 */
void CcdPhysicsController::RemoveCompoundChild(PHY_IPhysicsController *child)
{
	if (!child || !IsCompound()) {
		return;
	}
	// other controller must be a bullet controller too
	// verify that body and shape exist and match
	CcdPhysicsController *childCtrl = static_cast<CcdPhysicsController *>(child);
	btRigidBody *rootBody = GetRigidBody();
	btRigidBody *childBody = childCtrl->GetRigidBody();
	if (!rootBody || !childBody) {
		return;
	}
	const btCollisionShape *rootShape = rootBody->getCollisionShape();
	if (!rootShape ||
	    rootShape->getShapeType() != COMPOUND_SHAPE_PROXYTYPE) {
		return;
	}
	btCompoundShape *compoundShape = (btCompoundShape *)rootShape;
	// retrieve the shapeInfo
	CcdShapeConstructionInfo *childShapeInfo = childCtrl->GetShapeInfo();
	CcdShapeConstructionInfo *rootShapeInfo = GetShapeInfo();
	// and verify that the child is part of the parent
	int i = rootShapeInfo->FindChildShape(childShapeInfo, childCtrl);
	if (i < 0) {
		return;
	}
	rootShapeInfo->RemoveChildShape(i);
	if (childCtrl->m_bulletChildShape) {
		int numChildren = compoundShape->getNumChildShapes();
		for (i = 0; i < numChildren; i++) {
			if (compoundShape->getChildShape(i) == childCtrl->m_bulletChildShape) {
				compoundShape->removeChildShapeByIndex(i);
				compoundShape->recalculateLocalAabb();
				break;
			}
		}
		delete childCtrl->m_bulletChildShape;
		childCtrl->m_bulletChildShape = nullptr;
	}
	// recompute inertia of parent
	if (!rootBody->isStaticOrKinematicObject()) {
		btVector3 localInertia;
		float mass = 1.f / rootBody->getInvMass();
		compoundShape->calculateLocalInertia(mass, localInertia);
		rootBody->setMassProps(mass, localInertia * m_cci.m_inertiaFactor);
	}
	// must update the broadphase cache,
	m_cci.m_physicsEnv->RefreshCcdPhysicsController(this);
	// reactivate the children
	m_cci.m_physicsEnv->AddCcdPhysicsController(childCtrl);
}

PHY_IPhysicsController *CcdPhysicsController::GetReplica()
{
	CcdPhysicsController *replica = new CcdPhysicsController(*this);
	return replica;
}

// Keeping this separate for now, maybe we can combine it with GetReplica()...
PHY_IPhysicsController *CcdPhysicsController::GetReplicaForSensors()
{
	// This is used only to replicate Near and Radar sensor controllers
	// The replication of object physics controller is done in KX_BulletPhysicsController::GetReplica()
	CcdConstructionInfo cinfo = m_cci;

	if (m_collisionShape) {
		switch (m_collisionShape->getShapeType()) {
			case SPHERE_SHAPE_PROXYTYPE:
			{
				btSphereShape *orgShape = (btSphereShape *)m_collisionShape;
				cinfo.m_collisionShape = new btSphereShape(*orgShape);
				break;
			}

			case CONE_SHAPE_PROXYTYPE:
			{
				btConeShape *orgShape = (btConeShape *)m_collisionShape;
				cinfo.m_collisionShape = new btConeShape(*orgShape);
				break;
			}

			default:
			{
				return nullptr;
			}
		}
	}

	cinfo.m_MotionState = new DefaultMotionState();
	cinfo.m_shapeInfo = m_shapeInfo;

	CcdPhysicsController *replica = new CcdPhysicsController(cinfo);
	return replica;
}

bool CcdPhysicsController::IsPhysicsSuspended()
{
	return !GetPhysicsEnvironment()->IsActiveCcdPhysicsController(this);
}

/* Refresh the physics object from either an object or a mesh.
 * from_gameobj and from_meshobj can be nullptr
 *
 * when setting the mesh, the following vars get priority
 * 1) from_meshobj - creates the phys mesh from RAS_Mesh
 * 2) from_gameobj - creates the phys mesh from the DerivedMesh where possible, else the RAS_Mesh
 * 3) this - update the phys mesh from DerivedMesh or RAS_Mesh
 *
 * Most of the logic behind this is in m_shapeInfo->UpdateMesh(...)
 */
bool CcdPhysicsController::ReinstancePhysicsShape(KX_GameObject *from_gameobj, RAS_Mesh *from_meshobj, bool dupli)
{
	if (m_shapeInfo->m_shapeType != PHY_SHAPE_MESH) {
		return false;
	}

	if (!from_gameobj && !from_meshobj) {
		from_gameobj = KX_GameObject::GetClientObject((KX_ClientObjectInfo *)GetNewClientInfo());
	}

	if (dupli && (m_shapeInfo->GetRefCount() > 1)) {
		CcdShapeConstructionInfo *newShapeInfo = m_shapeInfo->GetReplica();
		m_shapeInfo->Release();
		m_shapeInfo = newShapeInfo;
	}

	/* updates the arrays used for making the new bullet mesh */
	m_shapeInfo->UpdateMesh(from_gameobj, from_meshobj);

	/* create the new bullet mesh */
	m_cci.m_physicsEnv->UpdateCcdPhysicsControllerShape(m_shapeInfo);

	return true;
}

void CcdPhysicsController::ReplacePhysicsShape(PHY_IPhysicsController *phyctrl)
{
	CcdShapeConstructionInfo *shapeInfo = ((CcdPhysicsController *)phyctrl)->GetShapeInfo();

	// switch shape info
	m_shapeInfo->Release();
	m_shapeInfo = shapeInfo->AddRef();

	// recreate Bullet shape only for this physics controller
	ReplaceControllerShape(nullptr);
	// refresh to remove collision pair
	m_cci.m_physicsEnv->RefreshCcdPhysicsController(this);
}

///////////////////////////////////////////////////////////
///A small utility class, DefaultMotionState
///
///////////////////////////////////////////////////////////

DefaultMotionState::DefaultMotionState()
{
	m_worldTransform.setIdentity();
	m_localScaling.setValue(1.0f, 1.0f, 1.0f);
}

DefaultMotionState::~DefaultMotionState()
{
}

mt::vec3 DefaultMotionState::GetWorldPosition() const
{
	return ToMt(m_worldTransform.getOrigin());
}

mt::vec3 DefaultMotionState::GetWorldScaling() const
{
	return ToMt(m_localScaling);
}

mt::mat3 DefaultMotionState::GetWorldOrientation() const
{
	return ToMt(m_worldTransform.getBasis());
}

void DefaultMotionState::SetWorldOrientation(const mt::mat3& ori)
{
	m_worldTransform.setBasis(ToBullet(ori));
}
void DefaultMotionState::SetWorldPosition(const mt::vec3& pos)
{
	m_worldTransform.setOrigin(ToBullet(pos));
}

void DefaultMotionState::SetWorldOrientation(const mt::quat& quat)
{
	m_worldTransform.setRotation(ToBullet(quat));
}

void DefaultMotionState::CalculateWorldTransformations()
{
}

// Shape constructor
CcdShapeConstructionInfo::MeshShapeMap CcdShapeConstructionInfo::m_meshShapeMap;

CcdShapeConstructionInfo *CcdShapeConstructionInfo::FindMesh(RAS_Mesh *mesh, RAS_Deformer *deformer, PHY_ShapeType shapeType)
{
	MeshShapeMap::const_iterator mit = m_meshShapeMap.find(MeshShapeKey(mesh, deformer, shapeType));
	if (mit != m_meshShapeMap.end()) {
		return mit->second;
	}
	return nullptr;
}

CcdShapeConstructionInfo *CcdShapeConstructionInfo::GetReplica()
{
	CcdShapeConstructionInfo *replica = new CcdShapeConstructionInfo(*this);
	replica->ProcessReplica();
	return replica;
}

void CcdShapeConstructionInfo::ProcessReplica()
{
	m_userData = nullptr;
	m_mesh = nullptr;
	m_triangleIndexVertexArray = nullptr;
	m_forceReInstance = false;
	m_shapeProxy = nullptr;
	m_vertexArray.clear();
	m_polygonIndexArray.clear();
	m_triFaceArray.clear();
	m_triFaceUVcoArray.clear();
	m_shapeArray.clear();
}

/* Updates the arrays used by CreateBulletShape(),
 * take care that recalcLocalAabb() runs after CreateBulletShape is called.
 * */
bool CcdShapeConstructionInfo::UpdateMesh(KX_GameObject *gameobj, RAS_Mesh *meshobj)
{
	if (!gameobj && !meshobj) {
		return false;
	}

	if (!ELEM(m_shapeType, PHY_SHAPE_MESH, PHY_SHAPE_POLYTOPE)) {
		return false;
	}

	RAS_Deformer *deformer = nullptr;

	// Specified mesh object is the highest priority.
	if (!meshobj) {
		// Object deformer is second priority.
		deformer = gameobj ? gameobj->GetDeformer() : nullptr;
		if (deformer) {
			meshobj = deformer->GetMesh();
		}
		else {
			// Object mesh is last priority.
			const std::vector<KX_Mesh *>& meshes = gameobj->GetMeshList();
			if (!meshes.empty()) {
				meshobj = meshes.front();
			}
		}
	}

	// Can't find the mesh object.
	if (!meshobj) {
		return false;
	}

	RAS_DisplayArrayList displayArrays;

	// Indices count.
	unsigned int numIndices = 0;
	// Original (without split of normal or UV) vertex count.
	unsigned int numVertices = 0;

	/// Absolute polygon start index for each used display arrays.
	std::vector<unsigned int> polygonStartIndices;
	unsigned int curPolygonStartIndex = 0;

	// Compute indices count and maximum vertex count.
	for (unsigned int i = 0, numMat = meshobj->GetNumMaterials(); i < numMat; ++i) {
		RAS_MeshMaterial *meshmat = meshobj->GetMeshMaterial(i);
		RAS_IPolyMaterial *mat = meshmat->GetBucket()->GetMaterial();

		RAS_DisplayArray *array = (deformer) ? deformer->GetDisplayArray(i) : meshmat->GetDisplayArray();
		const unsigned int indicesCount = array->GetTriangleIndexCount();

		// If collisions are disabled: do nothing.
		if (mat->IsCollider()) {
			numIndices += indicesCount;
			numVertices = std::max(numVertices, array->GetMaxOrigIndex() + 1);
			// Add valid display arrays.
			displayArrays.push_back(array);
			polygonStartIndices.push_back(curPolygonStartIndex);
		}

		curPolygonStartIndex += indicesCount / 3;
	}

	// Detect mesh without triangles.
	if (numIndices == 0 && m_shapeType == PHY_SHAPE_MESH) {
		return false;
	}

	m_vertexArray.resize(numVertices * 3);
	m_vertexRemap.resize(numVertices);
	// resize() doesn't initialize all values if the vector wasn't empty before. Prefer fill explicitly.
	std::fill(m_vertexRemap.begin(), m_vertexRemap.end(), -1);

	// Current vertex written.
	unsigned int curVert = 0;

	for (RAS_DisplayArray *array : displayArrays) {
		// Convert location of all vertices and remap if vertices weren't already converted.
		for (unsigned int j = 0, numvert = array->GetVertexCount(); j < numvert; ++j) {
			const RAS_VertexInfo& info = array->GetVertexInfo(j);
			const unsigned int origIndex = info.GetOrigIndex();
			/* Avoid double conversion of two unique vertices using the same base:
			 * using the same original vertex and so the same position.
			 */
			if (m_vertexRemap[origIndex] != -1) {
				continue;
			}

			const mt::vec3_packed& pos = array->GetPosition(j);
			m_vertexArray[curVert * 3] = pos.x;
			m_vertexArray[curVert * 3 + 1] = pos.y;
			m_vertexArray[curVert * 3 + 2] = pos.z;

			// Register the vertex index where the position was converted in m_vertexArray.
			m_vertexRemap[origIndex] = curVert++;
		}
	}

	// Convex shapes don't need indices.
	if (m_shapeType == PHY_SHAPE_MESH) {
		m_triFaceArray.resize(numIndices);
		m_triFaceUVcoArray.resize(numIndices);
		m_polygonIndexArray.resize(numIndices / 3);

		// Current triangle written.
		unsigned int curTri = 0;

		for (unsigned short i = 0, numArray = displayArrays.size(); i < numArray; ++i) {
			RAS_DisplayArray *array = displayArrays[i];
			const unsigned int polygonStartIndex = polygonStartIndices[i];

			// Convert triangles using remaped vertices index.
			for (unsigned int j = 0, numind = array->GetTriangleIndexCount(); j < numind; j += 3) {
				// Should match polygon access index with RAS_Mesh::GetPolygon.
				m_polygonIndexArray[curTri] = polygonStartIndex + j / 3;

				for (unsigned short k = 0; k < 3; ++k) {
					const unsigned int index = array->GetTriangleIndex(j + k);
					const unsigned int curInd = curTri * 3 + k;

					// Convert UV for raycast UV computation.
					const mt::vec2_packed& uv = array->GetUv(index, 0);
					m_triFaceUVcoArray[curInd] = {{uv.x, uv.y}};

					// Get vertex index from original index to m_vertexArray vertex index.
					const RAS_VertexInfo& info = array->GetVertexInfo(index);
					const unsigned int origIndex = info.GetOrigIndex();
					m_triFaceArray[curInd] = m_vertexRemap[origIndex];
				}
				++curTri;
			}
		}
	}

#if 0
	CM_Debug("# vert count " << m_vertexArray.size());
	for (int i = 0; i < m_vertexArray.size(); i += 3) {
		CM_Debug("v " << m_vertexArray[i] << " " << m_vertexArray[i + 1] << " " << m_vertexArray[i + 2]);
	}

	CM_Debug("# face count " << m_triFaceArray.size());
	for (int i = 0; i < m_triFaceArray.size(); i += 3) {
		CM_Debug("f " << m_triFaceArray[i] + 1 << " " << m_triFaceArray[i + 1] + 1 << " " << m_triFaceArray[i + 2] + 1);
	}
#endif

	// Force recreation of the m_triangleIndexVertexArray.
	if (m_triangleIndexVertexArray) {
		m_forceReInstance = true;
	}

	/* Make sure to also replace the mesh in the shape map! Otherwise we leave dangling references when we free.
	 * Note, this whole business could cause issues with shared meshes.
	 */
	for (MeshShapeMap::iterator it = m_meshShapeMap.begin(); it != m_meshShapeMap.end(); ) {
		if (it->second == this) {
			it = m_meshShapeMap.erase(it);
		}
		else {
			++it;
		}
	}

	// Register mesh object to shape.
	m_meshShapeMap[MeshShapeKey(meshobj, deformer, m_shapeType)] = this;

	m_mesh = meshobj;

	return true;
}

bool CcdShapeConstructionInfo::SetProxy(CcdShapeConstructionInfo *shapeInfo)
{
	if (!shapeInfo) {
		return false;
	}

	m_shapeType = PHY_SHAPE_PROXY;
	m_shapeProxy = shapeInfo;
	return true;
}

RAS_Mesh *CcdShapeConstructionInfo::GetMesh() const
{
	return m_mesh;
}

btCollisionShape *CcdShapeConstructionInfo::CreateBulletShape(btScalar margin, bool useGimpact, bool useBvh)
{
	btCollisionShape *collisionShape = nullptr;

	switch (m_shapeType) {
		case PHY_SHAPE_PROXY:
		{
			if (m_shapeProxy) {
				collisionShape = m_shapeProxy->CreateBulletShape(margin, useGimpact, useBvh);
			}
			break;
		}
		case PHY_SHAPE_BOX:
		{
			collisionShape = new btBoxShape(m_halfExtend);
			collisionShape->setMargin(margin);
			break;
		}
		case PHY_SHAPE_SPHERE:
		{
			collisionShape = new btSphereShape(m_radius);
			collisionShape->setMargin(margin);
			break;
		}
		case PHY_SHAPE_CYLINDER:
		{
			collisionShape = new btCylinderShapeZ(m_halfExtend);
			collisionShape->setMargin(margin);
			break;
		}
		case PHY_SHAPE_CONE:
		{
			collisionShape = new btConeShapeZ(m_radius, m_height);
			collisionShape->setMargin(margin);
			break;
		}
		case PHY_SHAPE_CAPSULE:
		{
			collisionShape = new btCapsuleShapeZ(m_radius, m_height);
			collisionShape->setMargin(margin);
			break;
		}
		case PHY_SHAPE_POLYTOPE:
		{
			if (m_vertexArray.size() == 0) {
				break;
			}

			collisionShape = new btConvexHullShape(&m_vertexArray[0], m_vertexArray.size() / 3, 3 * sizeof(btScalar));
			collisionShape->setMargin(margin);
			break;
		}
		case PHY_SHAPE_MESH:
		{
			if (m_vertexArray.size() == 0) {
				break;
			}

			// Let's use the latest btScaledBvhTriangleMeshShape: it allows true sharing of
			// triangle mesh information between duplicates => drastic performance increase when
			// duplicating complex mesh objects.
			// BUT it causes a small performance decrease when sharing is not required:
			// 9 multiplications/additions and one function call for each triangle that passes the mid phase filtering
			// One possible optimization is to use directly the btBvhTriangleMeshShape when the scale is 1,1,1
			// and btScaledBvhTriangleMeshShape otherwise.
			if (useGimpact) {
				if (!m_triangleIndexVertexArray || m_forceReInstance) {
					if (m_triangleIndexVertexArray) {
						delete m_triangleIndexVertexArray;
					}

					m_triangleIndexVertexArray = new btTriangleIndexVertexArray(
						m_triFaceArray.size() / 3,
						m_triFaceArray.data(),
						3 * sizeof(int),
						m_vertexArray.size() / 3,
						&m_vertexArray[0],
						3 * sizeof(btScalar));
					m_forceReInstance = false;
				}

				btGImpactMeshShape *gimpactShape = new btGImpactMeshShape(m_triangleIndexVertexArray);
				gimpactShape->setMargin(margin);
				gimpactShape->updateBound();
				collisionShape = gimpactShape;
			}
			else {
				if (!m_triangleIndexVertexArray || m_forceReInstance) {
					///enable welding, only for the objects that need it (such as soft bodies)
					if (0.0f != m_weldingThreshold1) {
						btTriangleMesh *collisionMeshData = new btTriangleMesh(true, false);
						collisionMeshData->m_weldingThreshold = m_weldingThreshold1;
						bool removeDuplicateVertices = true;
						// m_vertexArray not in multiple of 3 anymore, use m_triFaceArray
						for (unsigned int i = 0; i < m_triFaceArray.size(); i += 3) {
							btScalar *bt = &m_vertexArray[3 * m_triFaceArray[i]];
							btVector3 v1(bt[0], bt[1], bt[2]);
							bt = &m_vertexArray[3 * m_triFaceArray[i + 1]];
							btVector3 v2(bt[0], bt[1], bt[2]);
							bt = &m_vertexArray[3 * m_triFaceArray[i + 2]];
							btVector3 v3(bt[0], bt[1], bt[2]);
							collisionMeshData->addTriangle(v1, v2, v3, removeDuplicateVertices);
						}
						m_triangleIndexVertexArray = collisionMeshData;
					}
					else {
						if (m_triangleIndexVertexArray) {
							delete m_triangleIndexVertexArray;
						}
						m_triangleIndexVertexArray = new btTriangleIndexVertexArray(
							m_triFaceArray.size() / 3,
							m_triFaceArray.data(),
							3 * sizeof(int),
							m_vertexArray.size() / 3,
							&m_vertexArray[0],
							3 * sizeof(btScalar));
					}

					m_forceReInstance = false;
				}

				btBvhTriangleMeshShape *unscaledShape = new btBvhTriangleMeshShape(m_triangleIndexVertexArray, true, useBvh);
				unscaledShape->setMargin(margin);
				collisionShape = new btScaledBvhTriangleMeshShape(unscaledShape, btVector3(1.0f, 1.0f, 1.0f));
				collisionShape->setMargin(margin);
			}
			break;
		}
		case PHY_SHAPE_COMPOUND:
		{
			if (m_shapeArray.empty()) {
				break;
			}

			btCompoundShape *compoundShape = new btCompoundShape();
			for (CcdShapeConstructionInfo *childShape : m_shapeArray) {
				btCollisionShape *childCollisionShape = childShape->CreateBulletShape(margin, useGimpact, useBvh);
				if (childCollisionShape) {
					childCollisionShape->setLocalScaling(childShape->m_childScale);
					compoundShape->addChildShape(childShape->m_childTrans, childCollisionShape);
				}
			}

			collisionShape = compoundShape;
			break;
		}
		case PHY_SHAPE_EMPTY:
		{
			collisionShape = new btEmptyShape();
			collisionShape->setMargin(margin);
			break;
		}
		default:
		{
			BLI_assert(false);
		}
	}

	if (collisionShape) {
		collisionShape->setUserPointer(this);
	}

	return collisionShape;
}

void CcdShapeConstructionInfo::AddShape(CcdShapeConstructionInfo *shapeInfo)
{
	m_shapeArray.push_back(shapeInfo);
	shapeInfo->AddRef();
}

CcdShapeConstructionInfo::~CcdShapeConstructionInfo()
{
	for (CcdShapeConstructionInfo *shapeInfo : m_shapeArray) {
		shapeInfo->Release();
	}
	m_shapeArray.clear();

	if (m_triangleIndexVertexArray) {
		delete m_triangleIndexVertexArray;
	}
	m_vertexArray.clear();

	for (MeshShapeMap::iterator it = m_meshShapeMap.begin(); it != m_meshShapeMap.end(); ) {
		if (it->second == this) {
			it = m_meshShapeMap.erase(it);
		}
		else {
			++it;
		}
	}

	if (m_shapeType == PHY_SHAPE_PROXY && m_shapeProxy) {
		m_shapeProxy->Release();
	}
}

