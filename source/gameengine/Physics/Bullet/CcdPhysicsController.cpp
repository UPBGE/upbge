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

#include "RAS_DisplayArray.h"
#include "RAS_MeshObject.h"
#include "RAS_Polygon.h"

#include "KX_GameObject.h"

#include "BulletSoftBody/btSoftBody.h"
#include "BulletSoftBody/btSoftBodyInternals.h"
#include "BulletSoftBody/btSoftBodyHelpers.h"
#include "LinearMath/btConvexHull.h"
#include "BulletCollision/Gimpact/btGImpactShape.h"

#include "BulletSoftBody/btSoftRigidDynamicsWorld.h"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"

extern "C" {
	#include "BLI_utildefines.h"
	#include "BKE_cdderivedmesh.h"
	#include "BKE_global.h"
    #include "BKE_mesh_runtime.h"
    #include "BKE_layer.h"
    #include "BKE_object.h"
    #include "BKE_scene.h"
    #include "../depsgraph/DEG_depsgraph_query.h"
}


/// todo: fill all the empty CcdPhysicsController methods, hook them up to the btRigidBody class

//'temporarily' global variables
extern float gDeactivationTime;
extern bool gDisableDeactivation;

float gLinearSleepingTreshold;
float gAngularSleepingTreshold;

BlenderBulletCharacterController::BlenderBulletCharacterController(CcdPhysicsController *ctrl, btMotionState *motionState,
																   btPairCachingGhostObject *ghost, btConvexShape *shape, float stepHeight)
	:btKinematicCharacterController(ghost, shape, stepHeight, 2),
	m_ctrl(ctrl),
	m_motionState(motionState),
	m_jumps(0),
	m_maxJumps(1)
{
}

void BlenderBulletCharacterController::updateAction(btCollisionWorld *collisionWorld, btScalar dt)
{
	if (onGround())
		m_jumps = 0;

	btKinematicCharacterController::updateAction(collisionWorld, dt);
	m_motionState->setWorldTransform(getGhostObject()->getWorldTransform());
}

unsigned char BlenderBulletCharacterController::getMaxJumps() const
{
	return m_maxJumps;
}

void BlenderBulletCharacterController::setMaxJumps(unsigned char maxJumps)
{
	m_maxJumps = maxJumps;
}

unsigned char BlenderBulletCharacterController::getJumpCount() const
{
	return m_jumps;
}

bool BlenderBulletCharacterController::canJump() const
{
	return (onGround() && m_maxJumps > 0) || m_jumps < m_maxJumps;
}

void BlenderBulletCharacterController::jump()
{
	if (!canJump())
		return;

	m_verticalVelocity = m_jumpSpeed;
	m_wasJumping = true;
	m_jumps++;
}

const btVector3& BlenderBulletCharacterController::getWalkDirection()
{
	return m_walkDirection;
}

float BlenderBulletCharacterController::GetFallSpeed() const
{
	return m_fallSpeed;
}

void BlenderBulletCharacterController::SetFallSpeed(float fallSpeed)
{
	setFallSpeed(fallSpeed);
}

float BlenderBulletCharacterController::GetJumpSpeed() const
{
	return m_jumpSpeed;
}

void BlenderBulletCharacterController::SetJumpSpeed(float jumpSpeed)
{
	setJumpSpeed(jumpSpeed);
}

void BlenderBulletCharacterController::SetVelocity(const btVector3& vel, float time, bool local)
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

void BlenderBulletCharacterController::SetVelocity(const MT_Vector3& vel, float time, bool local)
{
	SetVelocity(ToBullet(vel), time, local);
}

void BlenderBulletCharacterController::Reset()
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
	m_prototypeTransformInitialized = false;
	m_softbodyMappingDone = false;
	m_collisionDelay = 0;
	m_newClientInfo = 0;
	m_registerCount = 0;
	m_softBodyTransformInitialized = false;
	m_parentCtrl = 0;
	// copy pointers locally to allow smart release
	m_MotionState = ci.m_MotionState;
	m_collisionShape = ci.m_collisionShape;
	// apply scaling before creating rigid body
	m_collisionShape->setLocalScaling(m_cci.m_scaling);
	if (m_cci.m_mass)
		m_collisionShape->calculateLocalInertia(m_cci.m_mass, m_cci.m_localInertiaTensor);
	// shape info is shared, increment ref count
	m_shapeInfo = ci.m_shapeInfo;
	if (m_shapeInfo)
		m_shapeInfo->AddRef();

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
	if (index == m_ccdConstraintRefs.size())
		m_ccdConstraintRefs.push_back(c);
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
	const MT_Vector3 pos = motionState->GetWorldPosition();
	const MT_Matrix3x3 mat = motionState->GetWorldOrientation();

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
		const MT_Vector3 pos = m_blenderMotionState->GetWorldPosition();
		const MT_Matrix3x3 mat = m_blenderMotionState->GetWorldOrientation();
		worldTrans.setOrigin(ToBullet(pos));
		worldTrans.setBasis(ToBullet(mat));
	}

	void setWorldTransform(const btTransform& worldTrans)
	{
		m_blenderMotionState->SetWorldPosition(ToMoto(worldTrans.getOrigin()));
		m_blenderMotionState->SetWorldOrientation(ToMoto(worldTrans.getRotation()));
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

#include "BulletSoftBody/btSoftBodyHelpers.h"

bool CcdPhysicsController::CreateSoftbody()
{
	int shapeType = m_cci.m_collisionShape ? m_cci.m_collisionShape->getShapeType() : 0;

	//disable soft body until first sneak preview is ready
	if (!m_cci.m_bSoft || !m_cci.m_collisionShape ||
	    ((shapeType != CONVEX_HULL_SHAPE_PROXYTYPE) &&
	     (shapeType != TRIANGLE_MESH_SHAPE_PROXYTYPE) &&
	     (shapeType != SCALED_TRIANGLE_MESH_SHAPE_PROXYTYPE)))
	{
		return false;
	}

	btRigidBody::btRigidBodyConstructionInfo rbci(m_cci.m_mass, m_bulletMotionState, m_collisionShape, m_cci.m_localInertiaTensor * m_cci.m_inertiaFactor);
	rbci.m_linearDamping = m_cci.m_linearDamping;
	rbci.m_angularDamping = m_cci.m_angularDamping;
	rbci.m_friction = m_cci.m_friction;
	rbci.m_restitution = m_cci.m_restitution;

	btVector3 p(0.0f, 0.0f, 0.0f);// = getOrigin();
	//btSoftBody*	psb=btSoftBodyHelpers::CreateRope(worldInfo,	btVector3(-10,0,i*0.25),btVector3(10,0,i*0.25),	16,1+2);
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

	//btSoftBody::Material*	pm=psb->appendMaterial();
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
		psb->generateBendingConstraints(2, pm);
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

	if (m_cci.m_soft_collisionflags & (CCD_BSB_COL_CL_RS + CCD_BSB_COL_CL_SS)) {
		psb->generateClusters(m_cci.m_soft_numclusteriterations);
	}

	psb->setTotalMass(m_cci.m_mass);

	psb->setCollisionFlags(0);

	///create a mapping between graphics mesh vertices and soft body vertices
	{
		RAS_MeshObject *rasMesh = GetShapeInfo()->GetMesh();

		if (rasMesh && !m_softbodyMappingDone) {
			RAS_MeshMaterial *mmat;

			//for each material
			for (int m = 0; m < rasMesh->NumMaterials(); m++) {
				mmat = rasMesh->GetMeshMaterial(m);

				RAS_IDisplayArray *array = mmat->GetDisplayArray();

				for (unsigned int i = 0, size = array->GetVertexCount(); i < size; ++i) {
					RAS_ITexVert *vertex = array->GetVertex(i);
					RAS_TexVertInfo& vertexInfo = array->GetVertexInfo(i);
					//search closest index, and store it in vertex
					vertexInfo.setSoftBodyIndex(0);
					btScalar maxDistSqr = 1e30;
					btSoftBody::tNodeArray& nodes(psb->m_nodes);
					btVector3 xyz = ToBullet(vertex->xyz());
					for (int n = 0; n < nodes.size(); n++) {
						btScalar distSqr = (nodes[n].m_x - xyz).length2();
						if (distSqr < maxDistSqr) {
							maxDistSqr = distSqr;

							vertexInfo.setSoftBodyIndex(n);
						}
					}
				}
			}
		}
	}
	m_softbodyMappingDone = true;

	btTransform startTrans;
	rbci.m_motionState->getWorldTransform(startTrans);

	m_MotionState->SetWorldPosition(ToMoto(startTrans.getOrigin()));
	m_MotionState->SetWorldOrientation(MT_Quaternion(0.0f, 0.0f, 0.0f, 1.0f));

	if (!m_prototypeTransformInitialized) {
		m_prototypeTransformInitialized = true;
		m_softBodyTransformInitialized = true;
		psb->transform(startTrans);
	}
	m_object->setCollisionFlags(m_object->getCollisionFlags() | m_cci.m_collisionFlags);
	if (m_cci.m_do_anisotropic)
		m_object->setAnisotropicFriction(m_cci.m_anisotropicFriction);
	return true;
}

bool CcdPhysicsController::CreateCharacterController()
{
	if (!m_cci.m_bCharacter)
		return false;

	m_object = new btPairCachingGhostObject();
	m_object->setCollisionShape(m_collisionShape);
	m_object->setCollisionFlags(btCollisionObject::CF_CHARACTER_OBJECT);

	btTransform trans;
	m_bulletMotionState->getWorldTransform(trans);
	m_object->setWorldTransform(trans);

	m_characterController = new BlenderBulletCharacterController(this, m_bulletMotionState, (btPairCachingGhostObject *)m_object,
																 (btConvexShape *)m_collisionShape, m_cci.m_stepHeight);

	m_characterController->setJumpSpeed(m_cci.m_jumpSpeed);
	m_characterController->setFallSpeed(m_cci.m_fallSpeed);
	m_characterController->setMaxJumps(m_cci.m_maxJumps);

	return true;
}

void CcdPhysicsController::CreateRigidbody()
{
	//btTransform trans = GetTransformFromMotionState(m_MotionState);
	m_bulletMotionState = new BlenderBulletMotionState(m_MotionState);

	///either create a btCollisionObject, btRigidBody or btSoftBody
	if (CreateSoftbody() || CreateCharacterController())
		// soft body created, done
		return;

	//create a rgid collision object
	btRigidBody::btRigidBodyConstructionInfo rbci(m_cci.m_mass, m_bulletMotionState, m_collisionShape, m_cci.m_localInertiaTensor * m_cci.m_inertiaFactor);
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
		GetCollisionObject()->setCollisionFlags(0);
		// sensor must never go to sleep: they need to detect continously
		GetCollisionObject()->setActivationState(DISABLE_DEACTIVATION);
	}
	GetCollisionObject()->setCollisionFlags(m_object->getCollisionFlags() | m_cci.m_collisionFlags);
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

MT_Vector3 CcdPhysicsController::GetGravity()
{
	MT_Vector3 gravity(0.0f, 0.0f, 0.0f);
	btRigidBody *body = GetRigidBody();
	if (body) {
		return ToMoto(body->getGravity());
	}
	return gravity;
}

void CcdPhysicsController::SetGravity(const MT_Vector3 &gravity)
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
		if (meshShape)
			delete meshShape;
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
	if (m_collisionShape)
		DeleteControllerShape();

	// If newShape is nullptr it means to create a new Bullet shape.
	if (!newShape)
		newShape = m_shapeInfo->CreateBulletShape(m_cci.m_margin, m_cci.m_bGimpact, !m_cci.m_bSoft);

	m_object->setCollisionShape(newShape);
	m_collisionShape = newShape;
	m_cci.m_collisionShape = newShape;

	btSoftBody *softBody = GetSoftBody();
	if (softBody) {
		btSoftRigidDynamicsWorld *world = GetPhysicsEnvironment()->GetDynamicsWorld();
		// remove the old softBody
		world->removeSoftBody(softBody);

		// soft body must be recreated
		delete m_object;
		m_object = nullptr;
		// force complete reinitialization
		m_softbodyMappingDone = false;
		m_prototypeTransformInitialized = false;
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
	if (m_cci.m_physicsEnv)
		m_cci.m_physicsEnv->RemoveCcdPhysicsController(this, true);

	if (m_MotionState)
		delete m_MotionState;
	if (m_bulletMotionState)
		delete m_bulletMotionState;
	if (m_characterController)
		delete m_characterController;
	delete m_object;

	DeleteControllerShape();

	if (m_shapeInfo) {
		m_shapeInfo->Release();
	}
}

void CcdPhysicsController::SimulationTick(float timestep)
{
	btRigidBody *body = GetRigidBody();
	if (!body || body->isStaticObject())
		return;

	// Clamp linear velocity
	if (m_cci.m_clamp_vel_max > 0.0f || m_cci.m_clamp_vel_min > 0.0f) {
		const btVector3 &linvel = body->getLinearVelocity();
		btScalar len = linvel.length();

		if (m_cci.m_clamp_vel_max > 0.0f && len > m_cci.m_clamp_vel_max)
			body->setLinearVelocity(linvel * (m_cci.m_clamp_vel_max / len));
		else if (m_cci.m_clamp_vel_min > 0.0f && !btFuzzyZero(len) && len < m_cci.m_clamp_vel_min)
			body->setLinearVelocity(linvel * (m_cci.m_clamp_vel_min / len));
	}

	// Clamp angular velocity
	if (m_cci.m_clamp_angvel_max > 0.0f || m_cci.m_clamp_angvel_min > 0.0f) {
		const btVector3 &angvel = body->getAngularVelocity();
		btScalar len = angvel.length();

		if (m_cci.m_clamp_angvel_max > 0.0f && len > m_cci.m_clamp_angvel_max)
			body->setAngularVelocity(angvel * (m_cci.m_clamp_angvel_max / len));
		else if (m_cci.m_clamp_angvel_min > 0.0f && !btFuzzyZero(len) && len < m_cci.m_clamp_angvel_min)
			body->setAngularVelocity(angvel * (m_cci.m_clamp_angvel_min / len));
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
			m_MotionState->SetWorldPosition(ToMoto(worldPos));
			m_MotionState->SetWorldOrientation(ToMoto(worldquat));
		}
		else {
			btVector3 aabbMin, aabbMax;
			sb->getAabb(aabbMin, aabbMax);
			btVector3 worldPos  = (aabbMax + aabbMin) * 0.5f;
			m_MotionState->SetWorldPosition(ToMoto(worldPos));
		}
		m_MotionState->CalculateWorldTransformations();
		return true;
	}

	btRigidBody *body = GetRigidBody();

	if (body && !body->isStaticObject()) {
		const btTransform& xform = body->getCenterOfMassTransform();
		const btMatrix3x3& worldOri = xform.getBasis();
		const btVector3& worldPos = xform.getOrigin();
		m_MotionState->SetWorldOrientation(ToMoto(worldOri));
		m_MotionState->SetWorldPosition(ToMoto(worldPos));
		m_MotionState->CalculateWorldTransformations();
	}

	const MT_Vector3& scale = m_MotionState->GetWorldScaling();
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
	SetParentCtrl((CcdPhysicsController *)parentctrl);
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

			if (m_cci.m_mass)
				m_collisionShape->calculateLocalInertia(m_cci.m_mass, m_cci.m_localInertiaTensor);
		}
	}

	// load some characterists that are not
	btRigidBody *oldbody = GetRigidBody();
	m_object = 0;
	CreateRigidbody();
	btRigidBody *body = GetRigidBody();
	if (body) {
		if (m_cci.m_mass) {
			body->setMassProps(m_cci.m_mass, m_cci.m_localInertiaTensor * m_cci.m_inertiaFactor);
		}

		if (oldbody) {
			body->setLinearFactor(oldbody->getLinearFactor());
			body->setAngularFactor(oldbody->getAngularFactor());
			if (oldbody->getActivationState() == DISABLE_DEACTIVATION)
				body->setActivationState(DISABLE_DEACTIVATION);
		}
	}
	// sensor object are added when needed
	if (!m_cci.m_bSensor)
		m_cci.m_physicsEnv->AddCcdPhysicsController(this);
}

void CcdPhysicsController::SetPhysicsEnvironment(class PHY_IPhysicsEnvironment *env)
{
	// can safely assume CCD environment
	CcdPhysicsEnvironment *physicsEnv = static_cast<CcdPhysicsEnvironment *>(env);

	if (m_cci.m_physicsEnv != physicsEnv) {
		// since the environment is changing, we must also move the controler to the
		// new environment. Note that we don't handle sensor explicitly: this
		// function can be called on sensor but only when they are not registered
		if (m_cci.m_physicsEnv->RemoveCcdPhysicsController(this, true))
		{
			physicsEnv->AddCcdPhysicsController(this);

			// Set the object to be active so it can at least by evaluated once.
			// This fixes issues with static objects not having their physics meshes
			// in the right spot when lib loading.
			this->GetCollisionObject()->setActivationState(ACTIVE_TAG);
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
void CcdPhysicsController::RelativeTranslate(const MT_Vector3& dlocin, bool local)
{
	if (m_object) {
		m_object->activate(true);
		if (m_object->isStaticObject()) {
			if (!m_cci.m_bSensor)
				m_object->setCollisionFlags(m_object->getCollisionFlags() | btCollisionObject::CF_KINEMATIC_OBJECT);
			// kinematic object should not set the transform, it disturbs the velocity interpolation
			return;
		}

		btVector3 dloc = ToBullet(dlocin);
		btTransform xform = m_object->getWorldTransform();

		if (local)
			dloc = xform.getBasis() * dloc;

		xform.setOrigin(xform.getOrigin() + dloc);
		SetCenterOfMassTransform(xform);
	}
}

void CcdPhysicsController::RelativeRotate(const MT_Matrix3x3& rotval, bool local)
{
	if (m_object) {
		m_object->activate(true);
		if (m_object->isStaticObject()) {
			if (!m_cci.m_bSensor)
				m_object->setCollisionFlags(m_object->getCollisionFlags() | btCollisionObject::CF_KINEMATIC_OBJECT);
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
	const MT_Matrix3x3 ori = m_MotionState->GetWorldOrientation();
	mat = ToBullet(ori);
}

MT_Matrix3x3 CcdPhysicsController::GetOrientation()
{
	const btMatrix3x3 orn = m_object->getWorldTransform().getBasis();
	return ToMoto(orn);
}

void CcdPhysicsController::SetOrientation(const MT_Matrix3x3& orn)
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

void CcdPhysicsController::SetPosition(const MT_Vector3& pos)
{
	if (m_object) {
		m_object->activate(true);
		if (m_object->isStaticObject()) {
			if (!m_cci.m_bSensor)
				m_object->setCollisionFlags(m_object->getCollisionFlags() | btCollisionObject::CF_KINEMATIC_OBJECT);
			// kinematic object should not set the transform, it disturbs the velocity interpolation
			return;
		}
		// not required, this function is only used to update the physic controller
		//m_MotionState->setWorldPosition(posX,posY,posZ);
		btTransform xform  = m_object->getWorldTransform();
		xform.setOrigin(ToBullet(pos));
		SetCenterOfMassTransform(xform);
		if (!m_softBodyTransformInitialized)
			m_softbodyStartTrans.setOrigin(xform.getOrigin());
		// not required
		//m_bulletMotionState->setWorldTransform(xform);
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

void CcdPhysicsController::ResolveCombinedVelocities(float linvelX, float linvelY, float linvelZ, float angVelX, float angVelY, float angVelZ)
{
}

void CcdPhysicsController::RefreshCollisions()
{
	// the object is in an inactive layer so it's useless to update it and can cause problems
	if (IsPhysicsSuspended())
		return;

	btSoftRigidDynamicsWorld *dw = GetPhysicsEnvironment()->GetDynamicsWorld();
	btBroadphaseProxy *proxy = m_object->getBroadphaseHandle();
	btDispatcher *dispatcher = dw->getDispatcher();
	btOverlappingPairCache *pairCache = dw->getPairCache();

	CleanPairCallback cleanPairs(proxy, pairCache, dispatcher);
	pairCache->processAllOverlappingPairs(&cleanPairs, dispatcher);

	// Forcibly recreate the physics object
	btBroadphaseProxy *handle = m_object->getBroadphaseHandle();
	GetPhysicsEnvironment()->UpdateCcdPhysicsController(this, GetMass(), m_object->getCollisionFlags(), handle->m_collisionFilterGroup, handle->m_collisionFilterMask);
}

void CcdPhysicsController::SuspendPhysics(bool freeConstraints)
{
	GetPhysicsEnvironment()->RemoveCcdPhysicsController(this, freeConstraints);
}

void CcdPhysicsController::RestorePhysics()
{
	GetPhysicsEnvironment()->AddCcdPhysicsController(this);
}

void CcdPhysicsController::SuspendDynamics(bool ghost)
{
	btRigidBody *body = GetRigidBody();
	if (body && !m_suspended && !GetConstructionInfo().m_bSensor && !IsPhysicsSuspended()) {
		btBroadphaseProxy *handle = body->getBroadphaseHandle();

		m_savedCollisionFlags = body->getCollisionFlags();
		m_savedMass = GetMass();
		m_savedDyna = m_cci.m_bDyna;
		m_savedCollisionFilterGroup = handle->m_collisionFilterGroup;
		m_savedCollisionFilterMask = handle->m_collisionFilterMask;
		m_suspended = true;
		GetPhysicsEnvironment()->UpdateCcdPhysicsController(this,
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
		GetPhysicsEnvironment()->UpdateCcdPhysicsController(this,
		                                                    m_savedMass,
		                                                    m_savedCollisionFlags,
		                                                    m_savedCollisionFilterGroup,
		                                                    m_savedCollisionFilterMask);
		body->activate();
		m_cci.m_bDyna = m_savedDyna;
		m_suspended = false;
	}
}

void CcdPhysicsController::GetPosition(MT_Vector3& pos) const
{
	const btTransform& xform = m_object->getWorldTransform();
	pos = ToMoto(xform.getOrigin());
}

void CcdPhysicsController::SetScaling(const MT_Vector3& scale)
{
	if (!btFuzzyZero(m_cci.m_scaling.x() - scale.x()) ||
	    !btFuzzyZero(m_cci.m_scaling.y() - scale.y()) ||
	    !btFuzzyZero(m_cci.m_scaling.z() - scale.z()))
	{
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
	const MT_Vector3 pos = m_MotionState->GetWorldPosition();
	const MT_Matrix3x3 rot = m_MotionState->GetWorldOrientation();
	ForceWorldTransform(ToBullet(rot), ToBullet(pos));

	if (!IsDynamic() && !GetConstructionInfo().m_bSensor && !GetCharacterController()) {
		btCollisionObject *object = GetRigidBody();
		object->setActivationState(ACTIVE_TAG);
		object->setCollisionFlags(object->getCollisionFlags() | btCollisionObject::CF_KINEMATIC_OBJECT);
	}
}

MT_Scalar CcdPhysicsController::GetMass()
{
	if (GetSoftBody())
		return GetSoftBody()->getTotalMass();

	MT_Scalar invmass = 0.0f;
	if (GetRigidBody())
		invmass = GetRigidBody()->getInvMass();
	if (invmass)
		return 1.0f / invmass;
	return 0.0f;
}

void CcdPhysicsController::SetMass(MT_Scalar newmass)
{
	btRigidBody *body = GetRigidBody();
	if (body && !m_suspended && !IsPhysicsSuspended() && (newmass > MT_EPSILON && GetMass() > MT_EPSILON)) {
		btBroadphaseProxy *handle = body->getBroadphaseHandle();
		GetPhysicsEnvironment()->UpdateCcdPhysicsController(this,
		                                                    newmass,
		                                                    body->getCollisionFlags(),
		                                                    handle->m_collisionFilterGroup,
		                                                    handle->m_collisionFilterMask);
	}
}

// physics methods
void CcdPhysicsController::ApplyTorque(const MT_Vector3&  torquein, bool local)
{
	btVector3 torque = ToBullet(torquein);
	btTransform xform = m_object->getWorldTransform();


	if (m_object && torque.length2() > (SIMD_EPSILON * SIMD_EPSILON)) {
		btRigidBody *body = GetRigidBody();
		m_object->activate();
		if (m_object->isStaticObject()) {
			if (!m_cci.m_bSensor)
				m_object->setCollisionFlags(m_object->getCollisionFlags() | btCollisionObject::CF_KINEMATIC_OBJECT);
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

void CcdPhysicsController::ApplyForce(const MT_Vector3& forcein, bool local)
{
	btVector3 force = ToBullet(forcein);

	if (m_object && force.length2() > (SIMD_EPSILON * SIMD_EPSILON)) {
		m_object->activate();
		if (m_object->isStaticObject()) {
			if (!m_cci.m_bSensor)
				m_object->setCollisionFlags(m_object->getCollisionFlags() | btCollisionObject::CF_KINEMATIC_OBJECT);
			return;
		}
		btTransform xform = m_object->getWorldTransform();

		if (local) {
			force = xform.getBasis() * force;
		}
		btRigidBody *body = GetRigidBody();
		if (body)
			body->applyCentralForce(force);
		btSoftBody *soft = GetSoftBody();
		if (soft) {
			// the force is applied on each node, must reduce it in the same extend
			if (soft->m_nodes.size() > 0)
				force /= soft->m_nodes.size();
			soft->addForce(force);
		}
	}
}
void CcdPhysicsController::SetAngularVelocity(const MT_Vector3& ang_vel, bool local)
{
	btVector3 angvel = ToBullet(ang_vel);

	/* Refuse tiny tiny velocities, as they might cause instabilities. */
	float vel_squared = angvel.length2();
	if (vel_squared > 0.0f && vel_squared <= (SIMD_EPSILON * SIMD_EPSILON))
		angvel = btVector3(0.0f, 0.0f, 0.0f);

	if (m_object) {
		m_object->activate(true);
		if (m_object->isStaticObject()) {
			if (!m_cci.m_bSensor)
				m_object->setCollisionFlags(m_object->getCollisionFlags() | btCollisionObject::CF_KINEMATIC_OBJECT);
			return;
		}
		btTransform xform = m_object->getWorldTransform();
		if (local) {
			angvel = xform.getBasis() * angvel;
		}
		btRigidBody *body = GetRigidBody();
		if (body)
			body->setAngularVelocity(angvel);
	}
}
void CcdPhysicsController::SetLinearVelocity(const MT_Vector3& lin_vel, bool local)
{
	btVector3 linVel = ToBullet(lin_vel);

	/* Refuse tiny tiny velocities, as they might cause instabilities. */
	float vel_squared = linVel.length2();
	if (vel_squared > 0.0f && vel_squared <= (SIMD_EPSILON * SIMD_EPSILON))
		linVel = btVector3(0.0f, 0.0f, 0.0f);

	if (m_object) {
		m_object->activate(true);
		if (m_object->isStaticObject()) {
			if (!m_cci.m_bSensor)
				m_object->setCollisionFlags(m_object->getCollisionFlags() | btCollisionObject::CF_KINEMATIC_OBJECT);
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
			if (body)
				body->setLinearVelocity(linVel);
		}
	}
}
void CcdPhysicsController::ApplyImpulse(const MT_Vector3& attach, const MT_Vector3& impulsein, bool local)
{
	btVector3 pos;
	btVector3 impulse = ToBullet(impulsein);

	if (m_object && impulse.length2() > (SIMD_EPSILON * SIMD_EPSILON)) {
		m_object->activate();
		if (m_object->isStaticObject()) {
			if (!m_cci.m_bSensor)
				m_object->setCollisionFlags(m_object->getCollisionFlags() | btCollisionObject::CF_KINEMATIC_OBJECT);
			return;
		}

		btTransform xform = m_object->getWorldTransform();

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
		if (body)
			body->applyImpulse(impulse, pos);
	}

}

void CcdPhysicsController::Jump()
{
	if (m_object && m_characterController)
		m_characterController->jump();
}

void CcdPhysicsController::SetActive(bool active)
{
}

float CcdPhysicsController::GetLinearDamping() const
{
	const btRigidBody *body = GetRigidBody();
	if (body)
		return body->getLinearDamping();
	return 0.0f;
}

float CcdPhysicsController::GetAngularDamping() const
{
	const btRigidBody *body = GetRigidBody();
	if (body)
		return body->getAngularDamping();
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
	if (!body)
		return;

	body->setDamping(linear, angular);
}

// reading out information from physics
MT_Vector3 CcdPhysicsController::GetLinearVelocity()
{
	btRigidBody *body = GetRigidBody();
	if (body) {
		const btVector3& linvel = body->getLinearVelocity();
		return ToMoto(linvel);
	}

	return MT_Vector3(0.0f, 0.0f, 0.0f);
}

MT_Vector3 CcdPhysicsController::GetAngularVelocity()
{
	btRigidBody *body = GetRigidBody();
	if (body) {
		const btVector3& angvel = body->getAngularVelocity();
		return ToMoto(angvel);
	}

	return MT_Vector3(0.0f, 0.0f, 0.0f);
}

MT_Vector3 CcdPhysicsController::GetVelocity(const MT_Vector3 &posin)
{
	btRigidBody *body = GetRigidBody();
	if (body) {
		btVector3 linvel = body->getVelocityInLocalPoint(ToBullet(posin));
		return ToMoto(linvel);
	}

	return MT_Vector3(0.0f, 0.0f, 0.0f);
}

MT_Vector3 CcdPhysicsController::GetLocalInertia()
{
	MT_Vector3 inertia(0.0f, 0.0f, 0.0f);
	btVector3 inv_inertia;
	if (GetRigidBody()) {
		inv_inertia = GetRigidBody()->getInvInertiaDiagLocal();
		if (!btFuzzyZero(inv_inertia.getX()) &&
		    !btFuzzyZero(inv_inertia.getY()) &&
		    !btFuzzyZero(inv_inertia.getZ()))
		{
			inertia = MT_Vector3(1.0f / inv_inertia.getX(), 1.0f / inv_inertia.getY(), 1.0f / inv_inertia.getZ());
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
		else
			body->setAngularFactor(m_cci.m_angularFactor);
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
		SG_Callbacks& callbacks = KX_GameObject::GetClientObject((KX_ClientObjectInfo *)clientinfo)->GetSGNode()->GetCallBackFunctions();
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
	if (child == nullptr || !IsCompound())
		return;
	// other controller must be a bullet controller too
	// verify that body and shape exist and match
	CcdPhysicsController *childCtrl = dynamic_cast<CcdPhysicsController *>(child);
	btRigidBody *rootBody = GetRigidBody();
	btRigidBody *childBody = childCtrl->GetRigidBody();
	if (!rootBody || !childBody)
		return;
	const btCollisionShape *rootShape = rootBody->getCollisionShape();
	const btCollisionShape *childShape = childBody->getCollisionShape();
	if (!rootShape ||
	    !childShape ||
	    rootShape->getShapeType() != COMPOUND_SHAPE_PROXYTYPE)
	{
		return;
	}
	btCompoundShape *compoundShape = (btCompoundShape *)rootShape;
	// compute relative transformation between parent and child
	btTransform rootTrans;
	btTransform childTrans;
	rootBody->getMotionState()->getWorldTransform(rootTrans);
	childBody->getMotionState()->getWorldTransform(childTrans);
	btVector3 rootScale = rootShape->getLocalScaling();
	rootScale[0] = 1.0/rootScale[0];
	rootScale[1] = 1.0/rootScale[1];
	rootScale[2] = 1.0/rootScale[2];
	// relative scale = child_scale/parent_scale
	btVector3 relativeScale = childShape->getLocalScaling() * rootScale;
	btMatrix3x3 rootRotInverse = rootTrans.getBasis().transpose();
	// relative pos = parent_rot^-1 * ((parent_pos-child_pos)/parent_scale)
	btVector3 relativePos = rootRotInverse * ((childTrans.getOrigin() - rootTrans.getOrigin()) * rootScale);
	// relative rot = parent_rot^-1 * child_rot
	btMatrix3x3 relativeRot = rootRotInverse * childTrans.getBasis();
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
	// recompute inertia of parent
	if (!rootBody->isStaticOrKinematicObject()) {
		btVector3 localInertia;
		float mass = 1.0f / rootBody->getInvMass();
		compoundShape->calculateLocalInertia(mass, localInertia);
		rootBody->setMassProps(mass, localInertia);
	}
	// must update the broadphase cache,
	GetPhysicsEnvironment()->RefreshCcdPhysicsController(this);
	// remove the children
	GetPhysicsEnvironment()->RemoveCcdPhysicsController(childCtrl, true);
}

/* Reverse function of the above, it will remove a shape from a compound shape
 * provided that the former was added to the later using  AddCompoundChild()
 */
void CcdPhysicsController::RemoveCompoundChild(PHY_IPhysicsController *child)
{
	if (child == nullptr || !IsCompound())
		return;
	// other controller must be a bullet controller too
	// verify that body and shape exist and match
	CcdPhysicsController *childCtrl = dynamic_cast<CcdPhysicsController *>(child);
	btRigidBody *rootBody = GetRigidBody();
	btRigidBody *childBody = childCtrl->GetRigidBody();
	if (!rootBody || !childBody)
		return;
	const btCollisionShape *rootShape = rootBody->getCollisionShape();
	if (!rootShape ||
	    rootShape->getShapeType() != COMPOUND_SHAPE_PROXYTYPE)
	{
		return;
	}
	btCompoundShape *compoundShape = (btCompoundShape *)rootShape;
	// retrieve the shapeInfo
	CcdShapeConstructionInfo *childShapeInfo = childCtrl->GetShapeInfo();
	CcdShapeConstructionInfo *rootShapeInfo = GetShapeInfo();
	// and verify that the child is part of the parent
	int i = rootShapeInfo->FindChildShape(childShapeInfo, childCtrl);
	if (i < 0)
		return;
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
		rootBody->setMassProps(mass, localInertia);
	}
	// must update the broadphase cache,
	GetPhysicsEnvironment()->RefreshCcdPhysicsController(this);
	// reactivate the children
	GetPhysicsEnvironment()->AddCcdPhysicsController(childCtrl);
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

	// Controllers used by sensors aren't using shape info.
	BLI_assert(m_shapeInfo);

	if (m_collisionShape) {
		switch (m_collisionShape->getShapeType())
		{
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
 * 1) from_meshobj - creates the phys mesh from RAS_MeshObject
 * 2) from_gameobj - creates the phys mesh from the DerivedMesh where possible, else the RAS_MeshObject
 * 3) this - update the phys mesh from DerivedMesh or RAS_MeshObject
 *
 * Most of the logic behind this is in m_shapeInfo->UpdateMesh(...)
 */
bool CcdPhysicsController::ReinstancePhysicsShape(KX_GameObject *from_gameobj, RAS_MeshObject *from_meshobj, bool dupli)
{
	if (m_shapeInfo->m_shapeType != PHY_SHAPE_MESH)
		return false;

	if (!from_gameobj && !from_meshobj)
		from_gameobj = KX_GameObject::GetClientObject((KX_ClientObjectInfo *)GetNewClientInfo());

	if (dupli && (m_shapeInfo->GetRefCount() > 1)) {
		CcdShapeConstructionInfo *newShapeInfo = m_shapeInfo->GetReplica();
		m_shapeInfo->Release();
		m_shapeInfo = newShapeInfo;
	}

	/* updates the arrays used for making the new bullet mesh */
	m_shapeInfo->UpdateMesh(from_gameobj);

	/* create the new bullet mesh */
	GetPhysicsEnvironment()->UpdateCcdPhysicsControllerShape(m_shapeInfo);

	return true;
}

bool CcdPhysicsController::ReinstancePhysicsShape2(RAS_MeshObject *meshobj, Object *ob, bool recalcGeom)
{
  if (m_shapeInfo->m_shapeType != PHY_SHAPE_MESH)
    return false;

  /* updates the arrays used for making the new bullet mesh */
  m_shapeInfo->SetMesh2(meshobj, ob, recalcGeom);

  /* create the new bullet mesh */
  GetPhysicsEnvironment()->UpdateCcdPhysicsControllerShape(m_shapeInfo);

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
	GetPhysicsEnvironment()->RefreshCcdPhysicsController(this);
}

void CcdPhysicsController::ReplicateConstraints(KX_GameObject *replica, std::vector<KX_GameObject *> constobj)
{
	if (replica->GetConstraints().size() == 0 || !replica->GetPhysicsController())
		return;

	PHY_IPhysicsEnvironment *physEnv = GetPhysicsEnvironment();

	std::vector<bRigidBodyJointConstraint *> constraints = replica->GetConstraints();
	std::vector<bRigidBodyJointConstraint *>::iterator consit;

	/* Object could have some constraints, iterate over all of theme to ensure that every constraint is recreated. */
	for (consit = constraints.begin(); consit != constraints.end(); ++consit) {
		/* Try to find the constraint targets in the list of group objects. */
		bRigidBodyJointConstraint *dat = (*consit);
		std::vector<KX_GameObject *>::iterator memit;
		for (memit = constobj.begin(); memit != constobj.end(); ++memit) {
			KX_GameObject *member = (*memit);
			/* If the group member is the actual target for the constraint. */
			if (dat->tar->id.name + 2 == member->GetName() && member->GetPhysicsController())
				physEnv->SetupObjectConstraints(replica, member, dat);
		}
	}
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

MT_Vector3 DefaultMotionState::GetWorldPosition() const
{
	return ToMoto(m_worldTransform.getOrigin());
}

MT_Vector3 DefaultMotionState::GetWorldScaling() const
{
	return ToMoto(m_localScaling);
}

MT_Matrix3x3 DefaultMotionState::GetWorldOrientation() const
{
	return ToMoto(m_worldTransform.getBasis());
}

void DefaultMotionState::SetWorldOrientation(const MT_Matrix3x3& ori)
{
	m_worldTransform.setBasis(ToBullet(ori));
}
void DefaultMotionState::SetWorldPosition(const MT_Vector3& pos)
{
	m_worldTransform.setOrigin(ToBullet(pos));
}

void DefaultMotionState::SetWorldOrientation(const MT_Quaternion& quat)
{
	m_worldTransform.setRotation(ToBullet(quat));
}

void DefaultMotionState::CalculateWorldTransformations()
{
}

// Shape constructor
std::map<RAS_MeshObject *, CcdShapeConstructionInfo *> CcdShapeConstructionInfo::m_meshShapeMap;

CcdShapeConstructionInfo *CcdShapeConstructionInfo::FindMesh(RAS_MeshObject *mesh, struct DerivedMesh *dm, bool polytope)
{
	if (polytope || dm)
		// not yet supported
		return nullptr;

	std::map<RAS_MeshObject *, CcdShapeConstructionInfo *>::const_iterator mit = m_meshShapeMap.find(mesh);
	if (mit != m_meshShapeMap.end())
		return mit->second;
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
	m_meshObject = nullptr;
	m_triangleIndexVertexArray = nullptr;
	m_forceReInstance = false;
	m_shapeProxy = nullptr;
	m_vertexArray.clear();
	m_polygonIndexArray.clear();
	m_triFaceArray.clear();
	m_triFaceUVcoArray.clear();
	m_shapeArray.clear();
}

bool CcdShapeConstructionInfo::SetMesh(RAS_MeshObject *meshobj, DerivedMesh *dm, bool polytope)
{
	int numpolys, numverts;

	// assume no shape information
	// no support for dynamic change of shape yet
	BLI_assert(IsUnused());
	m_shapeType = PHY_SHAPE_NONE;
	m_meshObject = nullptr;
	bool free_dm = false;

	// No mesh object or mesh has no polys
	if (!meshobj || !meshobj->HasColliderPolygon()) {
		m_vertexArray.clear();
		m_polygonIndexArray.clear();
		m_triFaceArray.clear();
		m_triFaceUVcoArray.clear();
		return false;
	}

	if (!dm) {
		free_dm = true;
		dm = CDDM_from_mesh(meshobj->GetMesh());
	}

	// Some meshes with modifiers returns 0 polys, call DM_ensure_tessface avoid this.
	DM_ensure_tessface(dm);

	MVert *mvert = dm->getVertArray(dm);
	MFace *mface = dm->getTessFaceArray(dm);
	numpolys = dm->getNumTessFaces(dm);
	numverts = dm->getNumVerts(dm);
	MTFace *tface = (MTFace *)dm->getTessFaceDataArray(dm, CD_MTFACE);

	/* double lookup */
	const int *index_mf_to_mpoly = (const int *)dm->getTessFaceDataArray(dm, CD_ORIGINDEX);
	const int *index_mp_to_orig  = (const int *)dm->getPolyDataArray(dm, CD_ORIGINDEX);
	if (!index_mf_to_mpoly) {
		index_mp_to_orig = nullptr;
	}

	m_shapeType = (polytope) ? PHY_SHAPE_POLYTOPE : PHY_SHAPE_MESH;

	// Convert blender geometry into bullet mesh, need these vars for mapping
	std::vector<bool> vert_tag_array(numverts, false);
	unsigned int tot_bt_verts = 0;

	if (polytope) {
		// Tag verts we're using
		for (int p2 = 0; p2 < numpolys; p2++) {
			MFace *mf = &mface[p2];
			const int origi = index_mf_to_mpoly ? DM_origindex_mface_mpoly(index_mf_to_mpoly, index_mp_to_orig, p2) : p2;
			RAS_Polygon *poly = (origi != ORIGINDEX_NONE) ? meshobj->GetPolygon(origi) : nullptr;

			// only add polygons that have the collision flag set
			if (poly && poly->IsCollider()) {
				if (!vert_tag_array[mf->v1]) {
					vert_tag_array[mf->v1] = true;
					tot_bt_verts++;
				}
				if (!vert_tag_array[mf->v2]) {
					vert_tag_array[mf->v2] = true;
					tot_bt_verts++;
				}
				if (!vert_tag_array[mf->v3]) {
					vert_tag_array[mf->v3] = true;
					tot_bt_verts++;
				}
				if (mf->v4 && !vert_tag_array[mf->v4]) {
					vert_tag_array[mf->v4] = true;
					tot_bt_verts++;
				}
			}
		}

		/* Can happen with ngons */
		if (!tot_bt_verts) {
			goto cleanup_empty_mesh;
		}

		m_vertexArray.resize(tot_bt_verts * 3);

		btScalar *bt = &m_vertexArray[0];

		for (int p2 = 0; p2 < numpolys; p2++) {
			MFace *mf = &mface[p2];
			const int origi = index_mf_to_mpoly ? DM_origindex_mface_mpoly(index_mf_to_mpoly, index_mp_to_orig, p2) : p2;
			RAS_Polygon *poly = (origi != ORIGINDEX_NONE) ? meshobj->GetPolygon(origi) : nullptr;

			// only add polygons that have the collisionflag set
			if (poly->IsCollider()) {
				if (vert_tag_array[mf->v1]) {
					const float *vtx = mvert[mf->v1].co;
					vert_tag_array[mf->v1] = false;
					*bt++ = vtx[0];
					*bt++ = vtx[1];
					*bt++ = vtx[2];
				}
				if (vert_tag_array[mf->v2]) {
					const float *vtx = mvert[mf->v2].co;
					vert_tag_array[mf->v2] = false;
					*bt++ = vtx[0];
					*bt++ = vtx[1];
					*bt++ = vtx[2];
				}
				if (vert_tag_array[mf->v3]) {
					const float *vtx = mvert[mf->v3].co;
					vert_tag_array[mf->v3] = false;
					*bt++ = vtx[0];
					*bt++ = vtx[1];
					*bt++ = vtx[2];
				}
				if (mf->v4 && vert_tag_array[mf->v4]) {
					const float *vtx = mvert[mf->v4].co;
					vert_tag_array[mf->v4] = false;
					*bt++ = vtx[0];
					*bt++ = vtx[1];
					*bt++ = vtx[2];
				}
			}
		}
	}
	else {
		unsigned int tot_bt_tris = 0;
		std::vector<int> vert_remap_array(numverts, 0);

		// Tag verts we're using
		for (int p2 = 0; p2 < numpolys; p2++) {
			MFace *mf = &mface[p2];
			const int origi = index_mf_to_mpoly ? DM_origindex_mface_mpoly(index_mf_to_mpoly, index_mp_to_orig, p2) : p2;
			RAS_Polygon *poly = (origi != ORIGINDEX_NONE) ? meshobj->GetPolygon(origi) : nullptr;

			// only add polygons that have the collision flag set
			if (poly && poly->IsCollider()) {
				if (!vert_tag_array[mf->v1]) {
					vert_tag_array[mf->v1] = true;
					vert_remap_array[mf->v1] = tot_bt_verts;
					tot_bt_verts++;
				}
				if (!vert_tag_array[mf->v2]) {
					vert_tag_array[mf->v2] = true;
					vert_remap_array[mf->v2] = tot_bt_verts;
					tot_bt_verts++;
				}
				if (!vert_tag_array[mf->v3]) {
					vert_tag_array[mf->v3] = true;
					vert_remap_array[mf->v3] = tot_bt_verts;
					tot_bt_verts++;
				}
				if (mf->v4 && !vert_tag_array[mf->v4]) {
					vert_tag_array[mf->v4] = true;
					vert_remap_array[mf->v4] = tot_bt_verts;
					tot_bt_verts++;
				}
				tot_bt_tris += (mf->v4 ? 2 : 1); /* a quad or a tri */
			}
		}

		/* Can happen with ngons */
		if (!tot_bt_verts) {
			goto cleanup_empty_mesh;
		}

		m_vertexArray.resize(tot_bt_verts * 3);
		m_polygonIndexArray.resize(tot_bt_tris);
		m_triFaceArray.resize(tot_bt_tris * 3);
		btScalar *bt = &m_vertexArray[0];
		int *poly_index_pt = &m_polygonIndexArray[0];
		int *tri_pt = &m_triFaceArray[0];

		UVco *uv_pt = nullptr;
		if (tface) {
			m_triFaceUVcoArray.resize(tot_bt_tris * 3);
			uv_pt = &m_triFaceUVcoArray[0];
		}
		else
			m_triFaceUVcoArray.clear();

		for (int p2 = 0; p2 < numpolys; p2++) {
			MFace *mf = &mface[p2];
			MTFace *tf = (tface) ? &tface[p2] : nullptr;
			const int origi = index_mf_to_mpoly ? DM_origindex_mface_mpoly(index_mf_to_mpoly, index_mp_to_orig, p2) : p2;
			RAS_Polygon *poly = (origi != ORIGINDEX_NONE) ? meshobj->GetPolygon(origi) : nullptr;

			// only add polygons that have the collisionflag set
			if (poly && poly->IsCollider()) {
				MVert *v1 = &mvert[mf->v1];
				MVert *v2 = &mvert[mf->v2];
				MVert *v3 = &mvert[mf->v3];

				// the face indices
				tri_pt[0] = vert_remap_array[mf->v1];
				tri_pt[1] = vert_remap_array[mf->v2];
				tri_pt[2] = vert_remap_array[mf->v3];
				tri_pt = tri_pt + 3;
				if (tf) {
					uv_pt[0].uv[0] = tf->uv[0][0];
					uv_pt[0].uv[1] = tf->uv[0][1];
					uv_pt[1].uv[0] = tf->uv[1][0];
					uv_pt[1].uv[1] = tf->uv[1][1];
					uv_pt[2].uv[0] = tf->uv[2][0];
					uv_pt[2].uv[1] = tf->uv[2][1];
					uv_pt += 3;
				}

				// m_polygonIndexArray
				*poly_index_pt = origi;
				poly_index_pt++;

				// the vertex location
				if (vert_tag_array[mf->v1]) { /* *** v1 *** */
					vert_tag_array[mf->v1] = false;
					*bt++ = v1->co[0];
					*bt++ = v1->co[1];
					*bt++ = v1->co[2];
				}
				if (vert_tag_array[mf->v2]) { /* *** v2 *** */
					vert_tag_array[mf->v2] = false;
					*bt++ = v2->co[0];
					*bt++ = v2->co[1];
					*bt++ = v2->co[2];
				}
				if (vert_tag_array[mf->v3]) { /* *** v3 *** */
					vert_tag_array[mf->v3] = false;
					*bt++ = v3->co[0];
					*bt++ = v3->co[1];
					*bt++ = v3->co[2];
				}

				if (mf->v4) {
					MVert *v4 = &mvert[mf->v4];

					tri_pt[0] = vert_remap_array[mf->v1];
					tri_pt[1] = vert_remap_array[mf->v3];
					tri_pt[2] = vert_remap_array[mf->v4];
					tri_pt = tri_pt + 3;
					if (tf) {
						uv_pt[0].uv[0] = tf->uv[0][0];
						uv_pt[0].uv[1] = tf->uv[0][1];
						uv_pt[1].uv[0] = tf->uv[2][0];
						uv_pt[1].uv[1] = tf->uv[2][1];
						uv_pt[2].uv[0] = tf->uv[3][0];
						uv_pt[2].uv[1] = tf->uv[3][1];
						uv_pt += 3;
					}

					// m_polygonIndexArray
					*poly_index_pt = origi;
					poly_index_pt++;

					// the vertex location
					if (vert_tag_array[mf->v4]) { // *** v4 ***
						vert_tag_array[mf->v4] = false;
						*bt++ = v4->co[0];
						*bt++ = v4->co[1];
						*bt++ = v4->co[2];
					}
				}
			}
		}

	// If this ever gets confusing, print out an OBJ file for debugging
#if 0
		CM_Debug("# vert count " << m_vertexArray.size());
		for (i = 0; i < m_vertexArray.size(); i += 1) {
			CM_Debug("v " << m_vertexArray[i].x() << " " << m_vertexArray[i].y() << " " << m_vertexArray[i].z());
		}

		CM_Debug("# face count " << m_triFaceArray.size());
		for (i = 0; i < m_triFaceArray.size(); i += 3) {
			CM_Debug("f " << m_triFaceArray[i] + 1 << " " <<  m_triFaceArray[i + 1] + 1 << " " <<  m_triFaceArray[i + 2] + 1);
		}
#endif
	}

#if 0
	if (validpolys == false) {
		// should not happen
		m_shapeType = PHY_SHAPE_NONE;
		return false;
	}
#endif

	m_meshObject = meshobj;
	if (free_dm) {
		dm->release(dm);
		dm = nullptr;
	}

	// sharing only on static mesh at present, if you change that, you must also change in FindMesh
	if (!polytope && !dm) {
		// triangle shape can be shared, store the mesh object in the map
		m_meshShapeMap.insert(std::pair<RAS_MeshObject *, CcdShapeConstructionInfo *>(meshobj, this));
	}
	return true;

cleanup_empty_mesh:
	m_shapeType = PHY_SHAPE_NONE;
	m_meshObject = nullptr;
	m_vertexArray.clear();
	m_polygonIndexArray.clear();
	m_triFaceArray.clear();
	m_triFaceUVcoArray.clear();
	if (free_dm) {
		dm->release(dm);
	}
	return false;
}

bool CcdShapeConstructionInfo::SetMesh2(RAS_MeshObject *meshobj, Object *ob, bool recalcGeom)
{
  int numpolys, numverts;

  // assume no shape information
  // no support for dynamic change of shape yet
  BLI_assert(IsUnused());
  m_shapeType = PHY_SHAPE_NONE;
  m_meshObject = nullptr;
  bool free_dm = false;

  // No mesh object or mesh has no polys
  if (!meshobj || !meshobj->HasColliderPolygon()) {
    m_vertexArray.clear();
    m_polygonIndexArray.clear();
	m_triFaceArray.clear();
	m_triFaceUVcoArray.clear();
	return false;
  }
  free_dm = true;
  Scene *scene = KX_GetActiveScene()->GetBlenderScene();
  ViewLayer *view_layer = BKE_view_layer_default_view(scene);
  Main *bmain = G_MAIN;
  Depsgraph *depsgraph = BKE_scene_get_depsgraph(bmain, scene, view_layer, false);

  Mesh *me = mesh_get_eval_final(depsgraph, scene, DEG_get_evaluated_object(depsgraph, ob), &CD_MASK_MESH);
  DerivedMesh *dm = CDDM_from_mesh(me);

	// Some meshes with modifiers returns 0 polys, call DM_ensure_tessface avoid this.
	DM_ensure_tessface(dm);

	MVert *mvert = dm->getVertArray(dm);
	MFace *mface = dm->getTessFaceArray(dm);
	numpolys = dm->getNumTessFaces(dm);
	numverts = dm->getNumVerts(dm);
	MTFace *tface = (MTFace *)dm->getTessFaceDataArray(dm, CD_MTFACE);

	/* double lookup */
	const int *index_mf_to_mpoly = (const int *)dm->getTessFaceDataArray(dm, CD_ORIGINDEX);
	const int *index_mp_to_orig  = (const int *)dm->getPolyDataArray(dm, CD_ORIGINDEX);
	if (!index_mf_to_mpoly) {
		index_mp_to_orig = nullptr;
	}

	m_shapeType = PHY_SHAPE_MESH;

	// Convert blender geometry into bullet mesh, need these vars for mapping
	std::vector<bool> vert_tag_array(numverts, false);
	unsigned int tot_bt_verts = 0;

	if (1) {
		unsigned int tot_bt_tris = 0;
		std::vector<int> vert_remap_array(numverts, 0);

		// Tag verts we're using
		for (int p2 = 0; p2 < numpolys; p2++) {
			MFace *mf = &mface[p2];
			const int origi = index_mf_to_mpoly ? DM_origindex_mface_mpoly(index_mf_to_mpoly, index_mp_to_orig, p2) : p2;
			RAS_Polygon *poly = (origi != ORIGINDEX_NONE) ? meshobj->GetPolygon(origi) : nullptr;

			// only add polygons that have the collision flag set
			if (poly && poly->IsCollider()) {
				if (!vert_tag_array[mf->v1]) {
					vert_tag_array[mf->v1] = true;
					vert_remap_array[mf->v1] = tot_bt_verts;
					tot_bt_verts++;
				}
				if (!vert_tag_array[mf->v2]) {
					vert_tag_array[mf->v2] = true;
					vert_remap_array[mf->v2] = tot_bt_verts;
					tot_bt_verts++;
				}
				if (!vert_tag_array[mf->v3]) {
					vert_tag_array[mf->v3] = true;
					vert_remap_array[mf->v3] = tot_bt_verts;
					tot_bt_verts++;
				}
				if (mf->v4 && !vert_tag_array[mf->v4]) {
					vert_tag_array[mf->v4] = true;
					vert_remap_array[mf->v4] = tot_bt_verts;
					tot_bt_verts++;
				}
				tot_bt_tris += (mf->v4 ? 2 : 1); /* a quad or a tri */
			}
		}

		/* Can happen with ngons */
		if (!tot_bt_verts) {
			goto cleanup_empty_mesh;
		}

		m_vertexArray.resize(tot_bt_verts * 3);
		m_polygonIndexArray.resize(tot_bt_tris);
		m_triFaceArray.resize(tot_bt_tris * 3);
		btScalar *bt = &m_vertexArray[0];
		int *poly_index_pt = &m_polygonIndexArray[0];
		int *tri_pt = &m_triFaceArray[0];

		UVco *uv_pt = nullptr;
		if (tface) {
			m_triFaceUVcoArray.resize(tot_bt_tris * 3);
			uv_pt = &m_triFaceUVcoArray[0];
		}
		else
			m_triFaceUVcoArray.clear();

		for (int p2 = 0; p2 < numpolys; p2++) {
			MFace *mf = &mface[p2];
			MTFace *tf = (tface) ? &tface[p2] : nullptr;
			const int origi = index_mf_to_mpoly ? DM_origindex_mface_mpoly(index_mf_to_mpoly, index_mp_to_orig, p2) : p2;
			RAS_Polygon *poly = (origi != ORIGINDEX_NONE) ? meshobj->GetPolygon(origi) : nullptr;

			// only add polygons that have the collisionflag set
			if (poly && poly->IsCollider()) {
				MVert *v1 = &mvert[mf->v1];
				MVert *v2 = &mvert[mf->v2];
				MVert *v3 = &mvert[mf->v3];

				// the face indices
				tri_pt[0] = vert_remap_array[mf->v1];
				tri_pt[1] = vert_remap_array[mf->v2];
				tri_pt[2] = vert_remap_array[mf->v3];
				tri_pt = tri_pt + 3;
				if (tf) {
					uv_pt[0].uv[0] = tf->uv[0][0];
					uv_pt[0].uv[1] = tf->uv[0][1];
					uv_pt[1].uv[0] = tf->uv[1][0];
					uv_pt[1].uv[1] = tf->uv[1][1];
					uv_pt[2].uv[0] = tf->uv[2][0];
					uv_pt[2].uv[1] = tf->uv[2][1];
					uv_pt += 3;
				}

				// m_polygonIndexArray
				*poly_index_pt = origi;
				poly_index_pt++;

				// the vertex location
				if (vert_tag_array[mf->v1]) { /* *** v1 *** */
					vert_tag_array[mf->v1] = false;
					*bt++ = v1->co[0];
					*bt++ = v1->co[1];
					*bt++ = v1->co[2];
				}
				if (vert_tag_array[mf->v2]) { /* *** v2 *** */
					vert_tag_array[mf->v2] = false;
					*bt++ = v2->co[0];
					*bt++ = v2->co[1];
					*bt++ = v2->co[2];
				}
				if (vert_tag_array[mf->v3]) { /* *** v3 *** */
					vert_tag_array[mf->v3] = false;
					*bt++ = v3->co[0];
					*bt++ = v3->co[1];
					*bt++ = v3->co[2];
				}

				if (mf->v4) {
					MVert *v4 = &mvert[mf->v4];

					tri_pt[0] = vert_remap_array[mf->v1];
					tri_pt[1] = vert_remap_array[mf->v3];
					tri_pt[2] = vert_remap_array[mf->v4];
					tri_pt = tri_pt + 3;
					if (tf) {
						uv_pt[0].uv[0] = tf->uv[0][0];
						uv_pt[0].uv[1] = tf->uv[0][1];
						uv_pt[1].uv[0] = tf->uv[2][0];
						uv_pt[1].uv[1] = tf->uv[2][1];
						uv_pt[2].uv[0] = tf->uv[3][0];
						uv_pt[2].uv[1] = tf->uv[3][1];
						uv_pt += 3;
					}

					// m_polygonIndexArray
					*poly_index_pt = origi;
					poly_index_pt++;

					// the vertex location
					if (vert_tag_array[mf->v4]) { // *** v4 ***
						vert_tag_array[mf->v4] = false;
						*bt++ = v4->co[0];
						*bt++ = v4->co[1];
						*bt++ = v4->co[2];
					}
				}
			}
		}

	// If this ever gets confusing, print out an OBJ file for debugging
#if 0
		CM_Debug("# vert count " << m_vertexArray.size());
		for (i = 0; i < m_vertexArray.size(); i += 1) {
			CM_Debug("v " << m_vertexArray[i].x() << " " << m_vertexArray[i].y() << " " << m_vertexArray[i].z());
		}

		CM_Debug("# face count " << m_triFaceArray.size());
		for (i = 0; i < m_triFaceArray.size(); i += 3) {
			CM_Debug("f " << m_triFaceArray[i] + 1 << " " <<  m_triFaceArray[i + 1] + 1 << " " <<  m_triFaceArray[i + 2] + 1);
		}
#endif
	}

#if 0
	if (validpolys == false) {
		// should not happen
		m_shapeType = PHY_SHAPE_NONE;
		return false;
	}
#endif

	m_meshObject = meshobj;
	if (free_dm) {
		//dm->release(dm);
		//dm = nullptr;
      BKE_object_free_derived_caches(ob);
	}

	if (recalcGeom) {
		DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
	}

	// sharing only on static mesh at present, if you change that, you must also change in FindMesh
	if (!dm) {
		// triangle shape can be shared, store the mesh object in the map
		m_meshShapeMap.insert(std::pair<RAS_MeshObject *, CcdShapeConstructionInfo *>(meshobj, this));
	}
	return true;

cleanup_empty_mesh:
	m_shapeType = PHY_SHAPE_NONE;
	m_meshObject = nullptr;
	m_vertexArray.clear();
	m_polygonIndexArray.clear();
	m_triFaceArray.clear();
	m_triFaceUVcoArray.clear();
	if (free_dm) {
		dm->release(dm);
	}
	return false;
}

#include <cstdio>

/* Updates the arrays used by CreateBulletShape(),
 * take care that recalcLocalAabb() runs after CreateBulletShape is called.
 * */
bool CcdShapeConstructionInfo::UpdateMesh(class KX_GameObject *gameobj)
{
	int numpolys;
	int numverts;

	unsigned int tot_bt_tris = 0;
	unsigned int tot_bt_verts = 0;

	int i, j;
	int v_orig;

	// Use for looping over verts in a face as a try or 2 tris
	const int quad_verts[7] =  {0, 1, 2, 0, 2, 3, -1};
	const int tri_verts[4] = {0, 1, 2, -1};
	const int *fv_pt;

	RAS_MeshObject *meshobj = gameobj->GetMesh(0);

	if (!gameobj && !meshobj)
		return false;

	if (m_shapeType != PHY_SHAPE_MESH)
		return false;

	DerivedMesh *dm = CDDM_from_mesh(meshobj->GetMesh());

	// get the mesh from the object if not defined
	if (!meshobj) {
		// modifier mesh
		if (dm)
			meshobj = gameobj->GetMesh(0);

		// game object first mesh
		if (!meshobj) {
			if (gameobj->GetMeshCount() > 0) {
				meshobj = gameobj->GetMesh(0);
			}
		}
	}

	if (dm && meshobj) {
		/*
		 * Derived Mesh Update
		 *
		 * */

		MVert *mvert = dm->getVertArray(dm);
		MFace *mface = dm->getTessFaceArray(dm);
		numpolys = dm->getNumTessFaces(dm);
		numverts = dm->getNumVerts(dm);

		// double lookup
		const int *index_mf_to_mpoly = (const int *)dm->getTessFaceDataArray(dm, CD_ORIGINDEX);
		const int *index_mp_to_orig = (const int *)dm->getPolyDataArray(dm, CD_ORIGINDEX);
		if (!index_mf_to_mpoly) {
			index_mp_to_orig = nullptr;
		}

		MFace *mf;
		MVert *mv;

		if (CustomData_has_layer(&dm->faceData, CD_MTFACE)) {
			MTFace *tface = (MTFace *)dm->getTessFaceDataArray(dm, CD_MTFACE);
			MTFace *tf;

			std::vector<bool> vert_tag_array(numverts, false);
			std::vector<int> vert_remap_array(numverts, 0);

			for (mf = mface, tf = tface, i = 0; i < numpolys; mf++, tf++, i++) {
				// 2.8x TODO: use GEMAT_NOPHYSICS.
				// if (tf->mode & TF_DYNAMIC)
				{
					int flen;

					if (mf->v4) {
						tot_bt_tris += 2;
						flen = 4;
					}
					else {
						tot_bt_tris++;
						flen = 3;
					}

					for (j = 0; j < flen; j++) {
						v_orig = (*(&mf->v1 + j));

						if (!vert_tag_array[v_orig]) {
							vert_tag_array[v_orig] = true;
							vert_remap_array[v_orig] = tot_bt_verts;
							tot_bt_verts++;
						}
					}
				}
			}

			m_vertexArray.resize(tot_bt_verts * 3);
			btScalar *bt = &m_vertexArray[0];

			m_triFaceArray.resize(tot_bt_tris * 3);
			int *tri_pt = &m_triFaceArray[0];

			m_triFaceUVcoArray.resize(tot_bt_tris * 3);
			UVco *uv_pt = &m_triFaceUVcoArray[0];

			m_polygonIndexArray.resize(tot_bt_tris);
			int *poly_index_pt = &m_polygonIndexArray[0];

			for (mf = mface, tf = tface, i = 0; i < numpolys; mf++, tf++, i++) {
				// 2.8x TODO: use GEMAT_NOPHYSICS.
				// if (tf->mode & TF_DYNAMIC)
				{
					int origi = index_mf_to_mpoly ? DM_origindex_mface_mpoly(index_mf_to_mpoly, index_mp_to_orig, i) : i;

					if (mf->v4) {
						fv_pt = quad_verts;
						*poly_index_pt++ = origi;
						*poly_index_pt++ = origi;
					}
					else {
						fv_pt = tri_verts;
						*poly_index_pt++ = origi;
					}

					for (; *fv_pt > -1; fv_pt++) {
						v_orig = (*(&mf->v1 + (*fv_pt)));

						if (vert_tag_array[v_orig]) {
							mv = mvert + v_orig;
							*bt++ = mv->co[0];
							*bt++ = mv->co[1];
							*bt++ = mv->co[2];

							vert_tag_array[v_orig] = false;
						}
						*tri_pt++ = vert_remap_array[v_orig];
						uv_pt->uv[0] = tf->uv[*fv_pt][0];
						uv_pt->uv[1] = tf->uv[*fv_pt][1];
						uv_pt++;
					}
				}
			}
		}
		else {
			// no need for a vertex mapping. simple/fast
			tot_bt_verts = numverts;

			for (mf = mface, i = 0; i < numpolys; mf++, i++) {
				tot_bt_tris += (mf->v4 ? 2 : 1);
			}

			m_vertexArray.resize(tot_bt_verts * 3);
			btScalar *bt = &m_vertexArray[0];

			m_triFaceArray.resize(tot_bt_tris * 3);
			int *tri_pt = &m_triFaceArray[0];

			m_polygonIndexArray.resize(tot_bt_tris);
			int *poly_index_pt = &m_polygonIndexArray[0];

			m_triFaceUVcoArray.clear();

			for (mv = mvert, i = 0; i < numverts; mv++, i++) {
				*bt++ = mv->co[0]; *bt++ = mv->co[1]; *bt++ = mv->co[2];
			}

			for (mf = mface, i = 0; i < numpolys; mf++, i++) {
				int origi = index_mf_to_mpoly ? DM_origindex_mface_mpoly(index_mf_to_mpoly, index_mp_to_orig, i) : i;

				if (mf->v4) {
					fv_pt = quad_verts;
					*poly_index_pt++ = origi;
					*poly_index_pt++ = origi;
				}
				else {
					fv_pt = tri_verts;
					*poly_index_pt++ = origi;
				}

				for (; *fv_pt > -1; fv_pt++)
					*tri_pt++ = (*(&mf->v1 + (*fv_pt)));
			}
		}
	}
	else {  /*
		     * RAS Mesh Update
		     *
		     * */
		// Note!, gameobj can be nullptr here

		/* transverts are only used for deformed RAS_Meshes, the RAS_TexVert data
		 * is too hard to get at, see below for details */
		float(*transverts)[3] = nullptr;

		// Tag verts we're using
		numpolys = meshobj->NumPolygons();
		numverts = meshobj->m_sharedvertex_map.size();
		const float *xyz;

		std::vector<bool> vert_tag_array(numverts, false);
		std::vector<int> vert_remap_array(numverts, 0);

		for (int p = 0; p < numpolys; p++) {
			RAS_Polygon *poly = meshobj->GetPolygon(p);
			if (poly->IsCollider()) {
				for (i = 0; i < poly->VertexCount(); i++) {
					v_orig = poly->GetVertexInfo(i).getOrigIndex();
					if (!vert_tag_array[v_orig]) {
						vert_tag_array[v_orig] = true;
						vert_remap_array[v_orig] = tot_bt_verts;
						tot_bt_verts++;
					}
				}
				tot_bt_tris += (poly->VertexCount() == 4 ? 2 : 1);
			}
		}

		// This case happens when none of the polys are colliders
		if (tot_bt_tris == 0 || tot_bt_verts == 0)
			return false;

		m_vertexArray.resize(tot_bt_verts * 3);
		btScalar *bt = &m_vertexArray[0];

		m_triFaceArray.resize(tot_bt_tris * 3);
		int *tri_pt = &m_triFaceArray[0];

		/* cant be used for anything useful in this case, since we don't rely on the original mesh
		 * will just be an array like pythons range(tot_bt_tris) */
		m_polygonIndexArray.resize(tot_bt_tris);

		int p = 0;
		int t = 0;
		while (t < tot_bt_tris) {
			RAS_Polygon *poly = meshobj->GetPolygon(p);

			if (poly->IsCollider()) {
				/* quad or tri loop */
				fv_pt = (poly->VertexCount() == 3 ? tri_verts : quad_verts);

				for (; *fv_pt > -1; fv_pt++) {
					v_orig = poly->GetVertexInfo(*fv_pt).getOrigIndex();
					if (vert_tag_array[v_orig]) {
						if (transverts) {
							/* deformed mesh, using RAS_TexVert locations would be too troublesome
							 * because they are use the gameob as a hash in the material slot */
							*bt++ = transverts[v_orig][0];
							*bt++ = transverts[v_orig][1];
							*bt++ = transverts[v_orig][2];
						}
						else {
							/* static mesh python may have modified */
							xyz = meshobj->GetVertexLocation(v_orig);
							*bt++ = xyz[0];
							*bt++ = xyz[1];
							*bt++ = xyz[2];
						}
						vert_tag_array[v_orig] = false;
					}
					*tri_pt++ = vert_remap_array[v_orig];
				}
			}
			// first triangle
			m_polygonIndexArray[t] = p;

			// if the poly is a quad we transform it in two triangles
			if (poly->VertexCount() == 4) {
				t++;
				// second triangle
				m_polygonIndexArray[t] = p;
			}
			t++;
			p++;
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

	/* force recreation of the m_triangleIndexVertexArray.
	 * If this has multiple users we cant delete */
	if (m_triangleIndexVertexArray) {
		m_forceReInstance = true;
	}

	// Make sure to also replace the mesh in the shape map! Otherwise we leave dangling references when we free.
	// Note, this whole business could cause issues with shared meshes. If we update one mesh, do we replace
	// them all?
	std::map<RAS_MeshObject *, CcdShapeConstructionInfo *>::iterator mit = m_meshShapeMap.find(m_meshObject);
	if (mit != m_meshShapeMap.end()) {
		m_meshShapeMap.erase(mit);
		m_meshShapeMap[meshobj] = this;
	}

	m_meshObject = meshobj;

	if (dm) {
		dm->needsFree = 1;
		dm->release(dm);
	}
	return true;
}

bool CcdShapeConstructionInfo::SetProxy(CcdShapeConstructionInfo *shapeInfo)
{
	if (shapeInfo == nullptr)
		return false;
	// no support for dynamic change
	BLI_assert(IsUnused());
	m_shapeType = PHY_SHAPE_PROXY;
	m_shapeProxy = shapeInfo;
	return true;
}

btCollisionShape *CcdShapeConstructionInfo::CreateBulletShape(btScalar margin, bool useGimpact, bool useBvh)
{
	btCollisionShape *collisionShape = nullptr;
	btCompoundShape *compoundShape = nullptr;

	if (m_shapeType == PHY_SHAPE_PROXY && m_shapeProxy != nullptr)
		return m_shapeProxy->CreateBulletShape(margin, useGimpact, useBvh);

	switch (m_shapeType)
	{
		default:
			break;

		case PHY_SHAPE_BOX:
			collisionShape = new btBoxShape(m_halfExtend);
			collisionShape->setMargin(margin);
			break;

		case PHY_SHAPE_SPHERE:
			collisionShape = new btSphereShape(m_radius);
			collisionShape->setMargin(margin);
			break;

		case PHY_SHAPE_CYLINDER:
			collisionShape = new btCylinderShapeZ(m_halfExtend);
			collisionShape->setMargin(margin);
			break;

		case PHY_SHAPE_CONE:
			collisionShape = new btConeShapeZ(m_radius, m_height);
			collisionShape->setMargin(margin);
			break;

		case PHY_SHAPE_POLYTOPE:
			collisionShape = new btConvexHullShape(&m_vertexArray[0], m_vertexArray.size() / 3, 3 * sizeof(btScalar));
			collisionShape->setMargin(margin);
			break;

		case PHY_SHAPE_CAPSULE:
			collisionShape = new btCapsuleShapeZ(m_radius, m_height);
			collisionShape->setMargin(margin);
			break;

		case PHY_SHAPE_MESH:
			// Let's use the latest btScaledBvhTriangleMeshShape: it allows true sharing of
			// triangle mesh information between duplicates => drastic performance increase when
			// duplicating complex mesh objects.
			// BUT it causes a small performance decrease when sharing is not required:
			// 9 multiplications/additions and one function call for each triangle that passes the mid phase filtering
			// One possible optimization is to use directly the btBvhTriangleMeshShape when the scale is 1,1,1
			// and btScaledBvhTriangleMeshShape otherwise.
			if (useGimpact) {
				if (!m_triangleIndexVertexArray || m_forceReInstance) {
					if (m_triangleIndexVertexArray)
						delete m_triangleIndexVertexArray;

					m_triangleIndexVertexArray = new btTriangleIndexVertexArray(
					    m_polygonIndexArray.size(),
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
						    m_polygonIndexArray.size(),
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

		case PHY_SHAPE_COMPOUND:
			if (m_shapeArray.size() > 0) {
				compoundShape = new btCompoundShape();
				for (std::vector<CcdShapeConstructionInfo *>::iterator sit = m_shapeArray.begin();
				     sit != m_shapeArray.end();
				     sit++)
				{
					collisionShape = (*sit)->CreateBulletShape(margin, useGimpact, useBvh);
					if (collisionShape) {
						collisionShape->setLocalScaling((*sit)->m_childScale);
						compoundShape->addChildShape((*sit)->m_childTrans, collisionShape);
					}
				}
				collisionShape = compoundShape;
			}
			break;
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
	for (std::vector<CcdShapeConstructionInfo *>::iterator sit = m_shapeArray.begin();
	     sit != m_shapeArray.end();
	     sit++)
	{
		(*sit)->Release();
	}
	m_shapeArray.clear();

	if (m_triangleIndexVertexArray)
		delete m_triangleIndexVertexArray;
	m_vertexArray.clear();
	if (m_shapeType == PHY_SHAPE_MESH && m_meshObject != nullptr) {
		std::map<RAS_MeshObject *, CcdShapeConstructionInfo *>::iterator mit = m_meshShapeMap.find(m_meshObject);
		if (mit != m_meshShapeMap.end() && mit->second == this) {
			m_meshShapeMap.erase(mit);
		}
	}
	if (m_shapeType == PHY_SHAPE_PROXY && m_shapeProxy != nullptr) {
		m_shapeProxy->Release();
	}
}

