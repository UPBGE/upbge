/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 * Game object wrapper
 */

/** \file gameengine/Ketsji/KX_GameObject.cpp
 *  \ingroup ketsji
 */

#include "KX_GameObject.h"
#include "KX_PythonComponent.h"
#include "KX_Camera.h" // only for their ::Type
#include "KX_LightObject.h"  // only for their ::Type
#include "KX_FontObject.h"  // only for their ::Type
#include "RAS_Mesh.h"
#include "RAS_MeshUser.h"
#include "RAS_BoundingBoxManager.h"
#include "RAS_Deformer.h"
#include "KX_NavMeshObject.h"
#include "KX_Mesh.h"
#include "KX_PolyProxy.h"
#include "SG_Controller.h"
#include "PHY_IGraphicController.h"
#include "SG_Node.h"
#include "SG_Familly.h"
#include "KX_ClientObjectInfo.h"
#include "RAS_BucketManager.h"
#include "KX_RayCast.h"
#include "KX_Globals.h"
#include "KX_PyMath.h"
#include "KX_NetworkMessageScene.h" //Needed for sendMessage()
#include "KX_ObstacleSimulation.h"
#include "KX_Scene.h"
#include "KX_LodLevel.h"
#include "KX_LodManager.h"
#include "KX_BoundingBox.h"
#include "SG_CullingNode.h"
#include "KX_BatchGroup.h"
#include "KX_CollisionContactPoints.h"

#include "BKE_object.h"

#include "BL_BlenderDataConversion.h" // For BL_ConvertDeformer.
#include "BL_ConvertObjectInfo.h"
#include "BL_ActionManager.h"
#include "BL_Action.h"

#include "EXP_PyObjectPlus.h" /* python stuff */
#include "EXP_ListWrapper.h"
#include "BLI_utildefines.h"

#ifdef WITH_PYTHON
#  include "EXP_PythonCallBack.h"
#  include "python_utildefines.h"
#endif

#include "EXP_PropFloat.h"

// Component stuff
#include "DNA_python_component_types.h"

// This file defines relationships between parents and children
// in the game engine.

#include "KX_NodeRelationships.h"

#include "BLI_math.h"

#include "CM_Message.h"

#define OB_MAX_COL_MASKS 20 // TODO

KX_GameObject::ActivityCullingInfo::ActivityCullingInfo()
	:m_flags(ACTIVITY_NONE),
	m_physicsRadius(0.0f),
	m_logicRadius(0.0f)
{
}

KX_GameObject::KX_GameObject(void *sgReplicationInfo,
                             SG_Callbacks callbacks)
	:m_clientInfo(this, KX_ClientObjectInfo::ACTOR),
	m_suspended(false),
	m_layer(0),
	m_lodManager(nullptr),
	m_currentLodLevel(0),
	m_meshUser(nullptr),
	m_convertInfo(nullptr),
	m_objectColor(mt::one4),
	m_bVisible(true),
	m_bOccluder(false),
	m_autoUpdateBounds(false),
	m_physicsController(nullptr),
	m_graphicController(nullptr),
	m_sgNode(new SG_Node(this, sgReplicationInfo, callbacks)),
	m_components(nullptr),
	m_instanceObjects(nullptr),
	m_dupliGroupObject(nullptr),
	m_actionManager(nullptr)
#ifdef WITH_PYTHON
	, m_collisionCallbacks(nullptr)
#endif
{
	// define the relationship between this node and it's parent.
	KX_NormalParentRelation *parent_relation = new KX_NormalParentRelation();
	m_sgNode->SetParentRelation(parent_relation);
}

KX_GameObject::KX_GameObject(const KX_GameObject& other)
	:m_clientInfo(this, other.m_clientInfo.m_type),
	m_name(other.m_name),
	m_layer(other.m_layer),
	m_meshes(other.m_meshes),
	m_lodManager(other.m_lodManager),
	m_currentLodLevel(0),
	m_meshUser(nullptr),
	m_convertInfo(other.m_convertInfo),
	m_objectColor(other.m_objectColor),
	m_bVisible(other.m_bVisible),
	m_bOccluder(other.m_bOccluder),
	m_activityCullingInfo(other.m_activityCullingInfo),
	m_autoUpdateBounds(other.m_autoUpdateBounds),
	m_physicsController(nullptr),
	m_graphicController(nullptr),
	m_sgNode(nullptr),
	m_components(nullptr),
	m_instanceObjects(nullptr),
	m_dupliGroupObject(nullptr),
	m_actionManager(nullptr)
#ifdef WITH_PYTHON
	, m_collisionCallbacks(other.m_collisionCallbacks)
#endif  // WITH_PYTHON
{
	if (m_lodManager) {
		m_lodManager->AddRef();
	}

#ifdef WITH_PYTHON
	Py_XINCREF(m_collisionCallbacks);

	if (other.m_components) {
		m_components.reset(other.m_components->GetReplica());

		for (KX_PythonComponent *component : m_components) {
			component->SetGameObject(this);
		}
	}
#endif  // WITH_PYTHON
}

KX_GameObject::~KX_GameObject()
{
#ifdef WITH_PYTHON
	// Unregister collision callbacks
	// Do this before we start freeing physics information like m_clientInfo
	if (m_collisionCallbacks) {
		UnregisterCollisionCallbacks();
		Py_CLEAR(m_collisionCallbacks);
	}
#endif // WITH_PYTHON

	RemoveMeshes();

	if (m_lodManager) {
		m_lodManager->Release();
	}
}

KX_GameObject *KX_GameObject::GetClientObject(KX_ClientObjectInfo *info)
{
	if (!info) {
		return nullptr;
	}
	return info->m_gameobject;
}

std::string KX_GameObject::GetName() const
{
	return m_name;
}

/* Set the name of the value */
void KX_GameObject::SetName(const std::string& name)
{
	m_name = name;
}

RAS_Deformer *KX_GameObject::GetDeformer()
{
	return (m_meshUser) ? m_meshUser->GetDeformer() : nullptr;
}

PHY_IPhysicsController *KX_GameObject::GetPhysicsController()
{
	return m_physicsController.get();
}

void KX_GameObject::SetPhysicsController(PHY_IPhysicsController *physicscontroller)
{
	m_physicsController.reset(physicscontroller);
}

PHY_IGraphicController *KX_GameObject::GetGraphicController()
{
	return m_graphicController.get();
}

void KX_GameObject::SetGraphicController(PHY_IGraphicController *graphiccontroller)
{
	m_graphicController.reset(graphiccontroller);
}

KX_GameObject *KX_GameObject::GetDupliGroupObject()
{
	return m_dupliGroupObject;
}

EXP_ListValue<KX_GameObject> *KX_GameObject::GetInstanceObjects()
{
	return m_instanceObjects.get();
}

void KX_GameObject::AddInstanceObjects(KX_GameObject *obj)
{
	if (!m_instanceObjects) {
		m_instanceObjects.reset(new EXP_ListValue<KX_GameObject>());
	}

	m_instanceObjects->Add(obj);
}

void KX_GameObject::RemoveInstanceObject(KX_GameObject *obj)
{
	BLI_assert(m_instanceObjects);
	m_instanceObjects->RemoveValue(obj);
}

void KX_GameObject::RemoveDupliGroupObject()
{
	if (m_dupliGroupObject) {
		m_dupliGroupObject = nullptr;
	}
}

void KX_GameObject::SetDupliGroupObject(KX_GameObject *obj)
{
	m_dupliGroupObject = obj;
}

const std::vector<bRigidBodyJointConstraint *>& KX_GameObject::GetConstraints()
{
	return m_convertInfo->m_constraints;
}

void KX_GameObject::ReplicateConstraints(PHY_IPhysicsEnvironment *physEnv, const std::vector<KX_GameObject *>& constobj)
{
	/*if (!m_physicsController || m_convertInfo->m_constraints.empty()) {
		return;
	}

	// Object could have some constraints, iterate over all of theme to ensure that every constraint is recreated.
	for (bRigidBodyJointConstraint *dat : m_convertInfo->m_constraints) {
		// Try to find the constraint targets in the list of group objects.
		for (KX_GameObject *member : constobj) {
			// If the group member is the actual target for the constraint.
			if ((dat->tar->id.name + 2) == member->GetName() && member->GetPhysicsController()) {
				physEnv->SetupObjectConstraints(this, member, dat);
			}
		}
	} TODO */
}

KX_GameObject *KX_GameObject::GetParent()
{
	KX_GameObject *result = nullptr;
	SG_Node *node = m_sgNode.get();

	while (node && !result)
	{
		node = node->GetParent();
		if (node) {
			result = (KX_GameObject *)node->GetClientObject();
		}
	}

	return result;
}

void KX_GameObject::SetParent(KX_GameObject *obj, bool addToCompound, bool ghost)
{
	// check on valid node in case a python controller holds a reference to a deleted object
	if (!obj) {
		return;
	}

	SG_Node *parentSgNode = obj->GetNode();
	KX_Scene *scene = GetScene();

	// Not already parented to same object, no parenting loop, not the object itself
	if (m_sgNode->GetParent() == parentSgNode || m_sgNode->IsAncessor(parentSgNode) || this == obj) {
		return;
	}

	if (!(scene->GetInactiveList().SearchValue(obj) != scene->GetObjectList().SearchValue(this))) {
		CM_FunctionWarning("child and parent are not in the same game objects list (active or inactive). This operation is forbidden.");
		return;
	}

	// Make sure the objects have some scale
	mt::vec3 scale1 = NodeGetWorldScaling();
	mt::vec3 scale2 = obj->NodeGetWorldScaling();
	if (mt::FuzzyZero(scale1) || mt::FuzzyZero(scale2)) {
		return;
	}

	// Remove us from our old parent and set our new parent
	RemoveParent();
	parentSgNode->AddChild(m_sgNode.get());

	if (m_physicsController) {
		m_physicsController->SuspendDynamics(ghost);
	}
	
	// Set us to our new scale, position, and orientation
	scale2[0] = 1.0f / scale2[0];
	scale2[1] = 1.0f / scale2[1];
	scale2[2] = 1.0f / scale2[2];
	scale1 = scale1 * scale2;

	const mt::mat3 invori = obj->NodeGetWorldOrientation().Inverse();
	const mt::vec3 newpos = invori * (NodeGetWorldPosition() - obj->NodeGetWorldPosition()) * scale2;

	NodeSetLocalScale(scale1);
	NodeSetLocalPosition(newpos);
	NodeSetLocalOrientation(invori * NodeGetWorldOrientation());
	NodeUpdate();

	// object will now be a child, it must be removed from the parent list
	EXP_ListValue<KX_GameObject>& rootlist = scene->GetRootParentList();
	rootlist.RemoveValue(this);

	// if the new parent is a compound object, add this object shape to the compound shape.
	// step 0: verify this object has physical controller
	if (m_physicsController && addToCompound) {
		// step 1: find the top parent (not necessarily obj)
		KX_GameObject *rootobj = (KX_GameObject *)parentSgNode->GetRootSGParent()->GetClientObject();
		// step 2: verify it has a physical controller and compound shape
		if (rootobj != nullptr &&
		    rootobj->m_physicsController != nullptr &&
		    rootobj->m_physicsController->IsCompound()) {
			rootobj->m_physicsController->AddCompoundChild(m_physicsController.get());
		}
	}
	// graphically, the object hasn't change place, no need to update m_graphicController
}

void KX_GameObject::RemoveParent()
{
	if (!m_sgNode->GetParent()) {
		return;
	}

	// get the root object to remove us from compound object if needed
	KX_GameObject *rootobj = (KX_GameObject *)m_sgNode->GetRootSGParent()->GetClientObject();
	// Set us to the right spot
	m_sgNode->SetLocalScale(m_sgNode->GetWorldScaling());
	m_sgNode->SetLocalOrientation(m_sgNode->GetWorldOrientation());
	m_sgNode->SetLocalPosition(m_sgNode->GetWorldPosition());

	// Remove us from our parent
	m_sgNode->DisconnectFromParent();
	NodeUpdate();

	KX_Scene *scene = GetScene();
	// the object is now a root object, add it to the parentlist
	EXP_ListValue<KX_GameObject>& rootlist = scene->GetRootParentList();
	if (!rootlist.SearchValue(this)) {
		// object was not in root list, add it now.
		rootlist.Add(this);
	}
	if (m_physicsController) {
		// in case this controller was added as a child shape to the parent
		if (rootobj &&
		    rootobj->m_physicsController &&
		    rootobj->m_physicsController->IsCompound()) {
			rootobj->m_physicsController->RemoveCompoundChild(m_physicsController.get());
		}
		m_physicsController->RestoreDynamics();
		if (m_physicsController->IsDynamic() && (rootobj && rootobj->m_physicsController)) {
			// dynamic object should remember the velocity they had while being parented
			const mt::vec3 childPoint = m_sgNode->GetWorldPosition();
			const mt::vec3 rootPoint = rootobj->m_sgNode->GetWorldPosition();
			const mt::vec3 relPoint = (childPoint - rootPoint);
			const mt::vec3 linVel = rootobj->m_physicsController->GetVelocity(relPoint);
			const mt::vec3 angVel = rootobj->m_physicsController->GetAngularVelocity();
			m_physicsController->SetLinearVelocity(linVel, false);
			m_physicsController->SetAngularVelocity(angVel, false);
		}
	}
	// graphically, the object hasn't change place, no need to update m_graphicController
}

BL_ActionManager *KX_GameObject::GetActionManager()
{
	// We only want to create an action manager if we need it
	if (!m_actionManager) {
		GetScene()->AddAnimatedObject(this);
		m_actionManager.reset(new BL_ActionManager(this));
	}
	return m_actionManager.get();
}

bool KX_GameObject::PlayAction(const std::string& name,
                               float start,
                               float end,
                               short layer,
                               short priority,
                               float blendin,
                               short play_mode,
                               float layer_weight,
                               short ipo_flags,
                               float playback_speed,
                               short blend_mode)
{
	return GetActionManager()->PlayAction(name, start, end, layer, priority, blendin, play_mode, layer_weight, ipo_flags, playback_speed, blend_mode);
}

void KX_GameObject::StopAction(short layer)
{
	GetActionManager()->StopAction(layer);
}

void KX_GameObject::RemoveTaggedActions()
{
	GetActionManager()->RemoveTaggedActions();
}

bool KX_GameObject::IsActionDone(short layer)
{
	return GetActionManager()->IsActionDone(layer);
}

bool KX_GameObject::IsActionsSuspended()
{
	return GetActionManager()->IsSuspended();
}

void KX_GameObject::UpdateActionManager(float curtime, bool applyToObject)
{
	GetActionManager()->Update(curtime, applyToObject);
}

float KX_GameObject::GetActionFrame(short layer)
{
	return GetActionManager()->GetActionFrame(layer);
}

const std::string KX_GameObject::GetActionName(short layer)
{
	return GetActionManager()->GetActionName(layer);
}

void KX_GameObject::SetActionFrame(short layer, float frame)
{
	GetActionManager()->SetActionFrame(layer, frame);
}

void KX_GameObject::SetPlayMode(short layer, short mode)
{
	GetActionManager()->SetPlayMode(layer, mode);
}

static void setGraphicController_recursive(SG_Node *node)
{
	const NodeList& children = node->GetChildren();

	for (SG_Node *childnode : children) {
		KX_GameObject *clientgameobj = static_cast<KX_GameObject *>(childnode->GetClientObject());
		if (clientgameobj != nullptr) { // This is a GameObject
			clientgameobj->ActivateGraphicController(false);
		}

		// if the childobj is nullptr then this may be an inverse parent link
		// so a non recursive search should still look down this node.
		setGraphicController_recursive(childnode);
	}
}


void KX_GameObject::ActivateGraphicController(bool recurse)
{
	if (m_graphicController) {
		m_graphicController->Activate(m_bVisible);
	}
	if (recurse) {
		setGraphicController_recursive(m_sgNode.get());
	}
}

void KX_GameObject::SetCollisionGroup(unsigned short group)
{
	if (m_physicsController) {
		m_physicsController->SetCollisionGroup(group);
		m_physicsController->RefreshCollisions();
	}
}
void KX_GameObject::SetCollisionMask(unsigned short mask)
{
	if (m_physicsController) {
		m_physicsController->SetCollisionMask(mask);
		m_physicsController->RefreshCollisions();
	}
}

unsigned short KX_GameObject::GetCollisionGroup() const
{
	return m_physicsController ? m_physicsController->GetCollisionGroup() : 0;
}
unsigned short KX_GameObject::GetCollisionMask() const
{
	return m_physicsController ? m_physicsController->GetCollisionMask() : 0;
}

EXP_Value *KX_GameObject::GetReplica()
{
	KX_GameObject *replica = new KX_GameObject(*this);

	// this will copy properties and so on...
	replica->ProcessReplica();

	return replica;
}

bool KX_GameObject::IsDynamic() const
{
	if (m_physicsController) {
		return m_physicsController->IsDynamic();
	}
	return false;
}

bool KX_GameObject::IsDynamicsSuspended() const
{
	if (m_physicsController) {
		return m_physicsController->IsDynamicsSuspended();
	}
	return false;
}

float KX_GameObject::GetLinearDamping() const
{
	if (m_physicsController) {
		return m_physicsController->GetLinearDamping();
	}
	return 0.0f;
}

float KX_GameObject::GetAngularDamping() const
{
	if (m_physicsController) {
		return m_physicsController->GetAngularDamping();
	}
	return 0.0f;
}

void KX_GameObject::SetLinearDamping(float damping)
{
	if (m_physicsController) {
		m_physicsController->SetLinearDamping(damping);
	}
}


void KX_GameObject::SetAngularDamping(float damping)
{
	if (m_physicsController) {
		m_physicsController->SetAngularDamping(damping);
	}
}

void KX_GameObject::SetDamping(float linear, float angular)
{
	if (m_physicsController) {
		m_physicsController->SetDamping(linear, angular);
	}
}

void KX_GameObject::ApplyForce(const mt::vec3& force, bool local)
{
	if (m_physicsController) {
		m_physicsController->ApplyForce(force, local);
	}
}

void KX_GameObject::ApplyTorque(const mt::vec3& torque, bool local)
{
	if (m_physicsController) {
		m_physicsController->ApplyTorque(torque, local);
	}
}

void KX_GameObject::ApplyMovement(const mt::vec3& dloc, bool local)
{
	if (m_physicsController) { // (IsDynamic())
		m_physicsController->RelativeTranslate(dloc, local);
	}
	m_sgNode->RelativeTranslate(dloc, m_sgNode->GetParent(), local);
	NodeUpdate();
}

void KX_GameObject::ApplyRotation(const mt::vec3& drot, bool local)
{
	mt::mat3 rotmat(drot);

	m_sgNode->RelativeRotate(rotmat, local);

	if (m_physicsController) { // (IsDynamic())
		m_physicsController->RelativeRotate(rotmat, local);
	}
	NodeUpdate();
}

void KX_GameObject::AddMeshUser()
{
	for (size_t i = 0; i < m_meshes.size(); ++i) {
		RAS_Deformer *deformer = BL_ConvertDeformer(this, m_meshes[i]);
		m_meshUser.reset(m_meshes[i]->AddMeshUser(&m_clientInfo, deformer));

		m_meshUser->SetMatrix(mt::mat4::FromAffineTransform(NodeGetWorldTransform()));
		m_meshUser->SetFrontFace(!IsNegativeScaling());
	}
}

void KX_GameObject::UpdateBuckets()
{
	// Update datas and add mesh slot to be rendered only if the object is not culled.
	if (m_sgNode->IsDirty(SG_Node::DIRTY_RENDER)) {
		m_meshUser->SetMatrix(mt::mat4::FromAffineTransform(NodeGetWorldTransform()));
		m_meshUser->SetFrontFace(!IsNegativeScaling());
		m_sgNode->ClearDirty(SG_Node::DIRTY_RENDER);
	}

	m_meshUser->SetLayer(m_layer);
	m_meshUser->SetColor(m_objectColor);
	m_meshUser->ActivateMeshSlots();
}

void KX_GameObject::ReplaceMesh(KX_Mesh *mesh, bool use_gfx, bool use_phys)
{
	if (use_gfx && mesh) {
		RemoveMeshes();
		AddMesh(mesh);
		AddMeshUser();
	}

	// Update the new assigned mesh with the physics mesh.
	if (use_phys && m_physicsController) {
		m_physicsController->ReinstancePhysicsShape(nullptr, use_gfx ? nullptr : mesh);
	}
	// Always make sure that the bounding box is updated to the new mesh.
	UpdateBounds(true);
}


void KX_GameObject::RemoveMeshes()
{
	// Remove all mesh slots.
	m_meshUser.reset(nullptr);
	m_meshes.clear();
}

const std::vector<KX_Mesh *>& KX_GameObject::GetMeshList() const
{
	return m_meshes;
}

RAS_MeshUser *KX_GameObject::GetMeshUser() const
{
	return m_meshUser.get();
}

bool KX_GameObject::Renderable(int layer) const
{
	return (m_meshUser != nullptr) && m_bVisible && (layer == 0 || m_layer & layer);
}

void KX_GameObject::SetLodManager(KX_LodManager *lodManager)
{
	// Reset lod level to avoid overflow index in KX_LodManager::GetLevel.
	m_currentLodLevel = 0;

	// Restore object original mesh.
	if (!lodManager && m_lodManager && m_lodManager->GetLevelCount() > 0) {
		KX_Mesh *origmesh = m_lodManager->GetLevel(0).GetMesh();
		ReplaceMesh(origmesh, true, false);
	}

	if (m_lodManager) {
		m_lodManager->Release();
	}

	m_lodManager = lodManager;

	if (m_lodManager) {
		m_lodManager->AddRef();
	}
}

KX_LodManager *KX_GameObject::GetLodManager() const
{
	return m_lodManager;
}

void KX_GameObject::UpdateLod(KX_Scene *scene, const mt::vec3& cam_pos, float lodfactor)
{
	if (!m_lodManager) {
		return;
	}

	const float distance2 = (NodeGetWorldPosition() - cam_pos).LengthSquared() * (lodfactor * lodfactor);
	const KX_LodLevel& lodLevel = m_lodManager->GetLevel(scene, m_currentLodLevel, distance2);

	KX_Mesh *mesh = lodLevel.GetMesh();
	if (mesh != m_meshes.front()) {
		ReplaceMesh(mesh, true, false);
	}

	m_currentLodLevel = lodLevel.GetLevel();
}

void KX_GameObject::UpdateActivity(float distance)
{
	// Manage physics culling.
	if (m_activityCullingInfo.m_flags & ActivityCullingInfo::ACTIVITY_PHYSICS) {
		if (distance > m_activityCullingInfo.m_physicsRadius) {
			SuspendPhysics(false);
		}
		else {
			RestorePhysics();
		}
	}

	// Manage logic culling.
	if (m_activityCullingInfo.m_flags & ActivityCullingInfo::ACTIVITY_LOGIC) {
		if (distance > m_activityCullingInfo.m_logicRadius) {
			SuspendLogic();
			if (m_actionManager) {
				m_actionManager->Suspend();
			}
		}
		else {
			ResumeLogic();
			if (m_actionManager) {
				m_actionManager->Resume();
			}
		}
	}
}

void KX_GameObject::UpdateTransform()
{
	// HACK: saves function call for dynamic object, they are handled differently
	if (m_physicsController && !m_physicsController->IsDynamic()) {
		m_physicsController->SetTransform();
	}
	if (m_graphicController) {
		// update the culling tree
		m_graphicController->SetGraphicTransform();
	}

}

void KX_GameObject::UpdateTransformFunc(SG_Node *node, void *gameobj, void *scene)
{
	((KX_GameObject *)gameobj)->UpdateTransform();
}

void KX_GameObject::SynchronizeTransform()
{
	// only used for sensor object, do full synchronization as bullet doesn't do it
	if (m_physicsController) {
		m_physicsController->SetTransform();
	}
	if (m_graphicController) {
		m_graphicController->SetGraphicTransform();
	}
}

void KX_GameObject::SynchronizeTransformFunc(SG_Node *node, void *gameobj, void *scene)
{
	((KX_GameObject *)gameobj)->SynchronizeTransform();
}

bool KX_GameObject::GetVisible(void)
{
	return m_bVisible;
}

static void setVisible_recursive(SG_Node *node, bool v)
{
	const NodeList& children = node->GetChildren();

	for (SG_Node *childnode : children) {
		KX_GameObject *clientgameobj = static_cast<KX_GameObject *>(childnode->GetClientObject());
		if (clientgameobj != nullptr) { // This is a GameObject
			clientgameobj->SetVisible(v, 0);
		}

		// if the childobj is nullptr then this may be an inverse parent link
		// so a non recursive search should still look down this node.
		setVisible_recursive(childnode, v);
	}
}


void KX_GameObject::SetVisible(bool v,
                               bool recursive)
{
	m_bVisible = v;
	if (m_graphicController) {
		m_graphicController->Activate(m_bVisible);
	}
	if (recursive) {
		setVisible_recursive(m_sgNode.get(), v);
	}
}

static void setOccluder_recursive(SG_Node *node, bool v)
{
	const NodeList& children = node->GetChildren();

	for (SG_Node *childnode : children) {
		KX_GameObject *clientgameobj = static_cast<KX_GameObject *>(childnode->GetClientObject());
		if (clientgameobj != nullptr) { // This is a GameObject
			clientgameobj->SetOccluder(v, false);
		}

		// if the childobj is nullptr then this may be an inverse parent link
		// so a non recursive search should still look down this node.
		setOccluder_recursive(childnode, v);
	}
}

void KX_GameObject::SetOccluder(bool v,
                                bool recursive)
{
	m_bOccluder = v;
	if (recursive) {
		setOccluder_recursive(m_sgNode.get(), v);
	}
}

void KX_GameObject::SuspendLogic()
{
	if (!m_suspended) {
		m_suspended = true;
	}
}

void KX_GameObject::ResumeLogic()
{
	if (m_suspended) {
		m_suspended = false;
	}
}

KX_GameObject::ObjectTypes KX_GameObject::GetObjectType() const
{
	return OBJECT_TYPE_OBJECT;
}

static void setDebug_recursive(KX_Scene *scene, SG_Node *node, bool debug)
{
	const NodeList& children = node->GetChildren();

	for (SG_Node *childnode : children) {
		KX_GameObject *clientgameobj = static_cast<KX_GameObject *>(childnode->GetClientObject());
		if (clientgameobj != nullptr) {
			if (debug) {
				if (!scene->ObjectInDebugList(clientgameobj)) {
					scene->AddObjectDebugProperties(clientgameobj);
				}
			}
			else {
				scene->RemoveObjectDebugProperties(clientgameobj);
			}
		}

		/* if the childobj is nullptr then this may be an inverse parent link
		 * so a non recursive search should still look down this node. */
		setDebug_recursive(scene, childnode, debug);
	}
}

void KX_GameObject::SetUseDebugProperties(bool debug, bool recursive)
{
	KX_Scene *scene = GetScene();

	if (debug) {
		if (!scene->ObjectInDebugList(this)) {
			scene->AddObjectDebugProperties(this);
		}
	}
	else {
		scene->RemoveObjectDebugProperties(this);
	}

	if (recursive) {
		setDebug_recursive(scene, m_sgNode.get(), debug);
	}
}

void KX_GameObject::SetLayer(int l)
{
	m_layer = l;
}

int KX_GameObject::GetLayer(void)
{
	return m_layer;
}

void KX_GameObject::AddLinearVelocity(const mt::vec3& lin_vel, bool local)
{
	if (m_physicsController) {
		const mt::vec3 lv = local ? NodeGetWorldOrientation() * lin_vel : lin_vel;
		m_physicsController->SetLinearVelocity(lv + m_physicsController->GetLinearVelocity(), 0);
	}
}

void KX_GameObject::SetLinearVelocity(const mt::vec3& lin_vel, bool local)
{
	if (m_physicsController) {
		m_physicsController->SetLinearVelocity(lin_vel, local);
	}
}

void KX_GameObject::SetAngularVelocity(const mt::vec3& ang_vel, bool local)
{
	if (m_physicsController) {
		m_physicsController->SetAngularVelocity(ang_vel, local);
	}
}

void KX_GameObject::SetObjectColor(const mt::vec4& rgbavec)
{
	m_objectColor = rgbavec;
}

const mt::vec4& KX_GameObject::GetObjectColor()
{
	return m_objectColor;
}

void KX_GameObject::AlignAxisToVect(const mt::vec3& dir, int axis, float fac)
{
	mt::mat3 orimat;
	mt::vec3 vect, ori, z, x, y;
	float len;

	vect = dir;
	len = vect.Length();
	if (mt::FuzzyZero(len)) {
		CM_FunctionError("null vector!");
		return;
	}

	if (fac <= 0.0f) {
		return;
	}

	// normalize
	vect /= len;
	orimat = NodeGetWorldOrientation();
	switch (axis) {
		case 0: // align x axis of new coord system to vect
		{ori = orimat.GetColumn(2);    // pivot axis
		 if (mt::FuzzyZero(1.0f - std::abs(mt::dot(vect, ori)))) {     // vect parallel to pivot?
			 ori = orimat.GetColumn(1);    // change the pivot!
		 }

		 if (fac == 1.0f) {
			 x = vect;
		 }
		 else {
			 x = (vect * fac) + ((orimat *mt::axisX3)*(1.0f - fac));
			 len = x.Length();
			 if (mt::FuzzyZero(len)) {
				 x = vect;
			 }
			 else {
				 x /= len;
			 }
		 }
		 y = mt::cross(ori, x);
		 z = mt::cross(x, y);
		 break;}
		case 1: // y axis
		{ori = orimat.GetColumn(0);
		 if (mt::FuzzyZero(1.0f - std::abs(mt::dot(vect, ori)))) {
			 ori = orimat.GetColumn(2);
		 }

		 if (fac == 1.0f) {
			 y = vect;
		 }
		 else {
			 y = (vect * fac) + ((orimat *mt::axisY3)*(1.0f - fac));
			 len = y.Length();
			 if (mt::FuzzyZero(len)) {
				 y = vect;
			 }
			 else {
				 y /= len;
			 }
		 }
		 z = mt::cross(ori, y);
		 x = mt::cross(y, z);
		 break;}
		case 2: // z axis
		{ori = orimat.GetColumn(1);
		 if (mt::FuzzyZero(1.0f - std::abs(mt::dot(vect, ori)))) {
			 ori = orimat.GetColumn(0);
		 }

		 if (fac == 1.0f) {
			 z = vect;
		 }
		 else {
			 z = (vect * fac) + ((orimat *mt::axisZ3)*(1.0f - fac));
			 len = z.Length();
			 if (mt::FuzzyZero(len)) {
				 z = vect;
			 }
			 else {
				 z /= len;
			 }
		 }
		 x = mt::cross(ori, z);
		 y = mt::cross(z, x);
		 break;}
		default: // invalid axis specified
			CM_FunctionWarning("invalid axis '" << axis << "'");
			return;
	}
	x.Normalize(); // normalize the new base vectors
	y.Normalize();
	z.Normalize();
	orimat = mt::mat3(x, y, z);

	if (m_sgNode->GetParent() != nullptr) {
		// the object is a child, adapt its local orientation so that
		// the global orientation is aligned as we want (cancelling out the parent orientation)
		mt::mat3 invori = m_sgNode->GetParent()->GetWorldOrientation().Inverse();
		NodeSetLocalOrientation(invori * orimat);
	}
	else {
		NodeSetLocalOrientation(orimat);
	}
}

float KX_GameObject::GetMass()
{
	if (m_physicsController) {
		return m_physicsController->GetMass();
	}
	return 0.0f;
}

mt::vec3 KX_GameObject::GetLocalInertia()
{
	mt::vec3 local_inertia = mt::zero3;
	if (m_physicsController) {
		local_inertia = m_physicsController->GetLocalInertia();
	}
	return local_inertia;
}

mt::vec3 KX_GameObject::GetLinearVelocity(bool local)
{
	mt::vec3 velocity = mt::zero3, locvel;
	mt::mat3 ori;
	if (m_physicsController) {
		velocity = m_physicsController->GetLinearVelocity();

		if (local) {
			ori = NodeGetWorldOrientation();

			locvel = velocity * ori;
			return locvel;
		}
	}
	return velocity;
}

mt::vec3 KX_GameObject::GetAngularVelocity(bool local)
{
	mt::vec3 velocity = mt::zero3, locvel;
	mt::mat3 ori;
	if (m_physicsController) {
		velocity = m_physicsController->GetAngularVelocity();

		if (local) {
			ori = NodeGetWorldOrientation();

			locvel = velocity * ori;
			return locvel;
		}
	}
	return velocity;
}

mt::vec3 KX_GameObject::GetGravity() const
{
	if (!m_physicsController) {
		return mt::zero3;
	}

	return m_physicsController->GetGravity();
}

void KX_GameObject::SetGravity(const mt::vec3 &gravity)
{
	if (m_physicsController) {
		m_physicsController->SetGravity(gravity);
	}
}

mt::vec3 KX_GameObject::GetVelocity(const mt::vec3& point)
{
	if (m_physicsController) {
		return m_physicsController->GetVelocity(point);
	}
	return mt::zero3;
}

void KX_GameObject::NodeSetLocalPosition(const mt::vec3& trans)
{
	if (m_physicsController && !m_sgNode->GetParent()) {
		// don't update physic controller if the object is a child:
		// 1) the transformation will not be right
		// 2) in this case, the physic controller is necessarily a static object
		//    that is updated from the normal kinematic synchronization
		m_physicsController->SetPosition(trans);
	}

	m_sgNode->SetLocalPosition(trans);
}

void KX_GameObject::NodeSetLocalOrientation(const mt::mat3& rot)
{
	if (m_physicsController && !m_sgNode->GetParent()) {
		// see note above
		m_physicsController->SetOrientation(rot);
	}
	m_sgNode->SetLocalOrientation(rot);
}

void KX_GameObject::NodeSetGlobalOrientation(const mt::mat3& rot)
{
	SG_Node *parentSgNode = m_sgNode->GetParent();
	if (parentSgNode) {
		NodeSetLocalOrientation(parentSgNode->GetWorldOrientation().Inverse() * rot);
	}
	else {
		NodeSetLocalOrientation(rot);
	}
}

void KX_GameObject::NodeSetLocalScale(const mt::vec3& scale)
{
	if (m_physicsController && !m_sgNode->GetParent()) {
		m_physicsController->SetScaling(scale);
	}
	m_sgNode->SetLocalScale(scale);
}

void KX_GameObject::NodeSetRelativeScale(const mt::vec3& scale)
{
	m_sgNode->RelativeScale(scale);
	if (m_physicsController && (!m_sgNode->GetParent())) {
		// see note above
		// we can use the local scale: it's the same thing for a root object
		// and the world scale is not yet updated
		const mt::vec3& newscale = NodeGetLocalScaling();
		m_physicsController->SetScaling(newscale);
	}
}

void KX_GameObject::NodeSetWorldScale(const mt::vec3& scale)
{
	SG_Node *parent = m_sgNode->GetParent();
	if (parent) {
		// Make sure the objects have some scale
		mt::vec3 p_scale = parent->GetWorldScaling();
		if (mt::FuzzyZero(p_scale)) {
			return;
		}

		p_scale[0] = 1.0f / p_scale[0];
		p_scale[1] = 1.0f / p_scale[1];
		p_scale[2] = 1.0f / p_scale[2];

		NodeSetLocalScale(scale * p_scale);
	}
	else {
		NodeSetLocalScale(scale);
	}
}

void KX_GameObject::NodeSetWorldPosition(const mt::vec3& trans)
{
	SG_Node *parent = m_sgNode->GetParent();
	if (parent != nullptr) {
		// Make sure the objects have some scale
		mt::vec3 scale = parent->GetWorldScaling();
		if (mt::FuzzyZero(scale)) {
			return;
		}

		scale[0] = 1.0f / scale[0];
		scale[1] = 1.0f / scale[1];
		scale[2] = 1.0f / scale[2];

		const mt::mat3 invori = parent->GetWorldOrientation().Inverse();
		const mt::vec3 newpos = invori * (trans - parent->GetWorldPosition()) * scale;
		NodeSetLocalPosition(newpos);
	}
	else {
		NodeSetLocalPosition(trans);
	}
}

void KX_GameObject::NodeUpdate()
{
	m_sgNode->UpdateWorldData();
}

const mt::mat3& KX_GameObject::NodeGetWorldOrientation() const
{
	return m_sgNode->GetWorldOrientation();
}

const mt::mat3& KX_GameObject::NodeGetLocalOrientation() const
{
	return m_sgNode->GetLocalOrientation();
}

const mt::vec3& KX_GameObject::NodeGetWorldScaling() const
{
	return m_sgNode->GetWorldScaling();
}

const mt::vec3& KX_GameObject::NodeGetLocalScaling() const
{
	return m_sgNode->GetLocalScale();
}

const mt::vec3& KX_GameObject::NodeGetWorldPosition() const
{
	return m_sgNode->GetWorldPosition();
}

const mt::vec3& KX_GameObject::NodeGetLocalPosition() const
{
	return m_sgNode->GetLocalPosition();
}

mt::mat3x4 KX_GameObject::NodeGetWorldTransform() const
{
	return m_sgNode->GetWorldTransform();
}

mt::mat3x4 KX_GameObject::NodeGetLocalTransform() const
{
	return m_sgNode->GetLocalTransform();
}

Object *KX_GameObject::GetBlenderObject() const
{
	// Non converted objects has default camera doesn't have convert info.
	return (m_convertInfo) ? m_convertInfo->m_blenderObject : nullptr;
}

BL_ConvertObjectInfo *KX_GameObject::GetConvertObjectInfo() const
{
	return m_convertInfo;
}

void KX_GameObject::SetConvertObjectInfo(BL_ConvertObjectInfo *info)
{
	m_convertInfo = info;
}

void KX_GameObject::SetNode(SG_Node *node)
{
	m_sgNode.reset(node);
}

void KX_GameObject::UpdateBounds(bool force)
{
	if ((!m_autoUpdateBounds && !force) || !m_meshUser) {
		return;
	}

	RAS_BoundingBox *boundingBox = m_meshUser->GetBoundingBox();
	if (!boundingBox || (!boundingBox->GetModified() && !force)) {
		return;
	}

	RAS_Deformer *deformer = GetDeformer();
	if (deformer) {
		/** Update all the deformer, not only per material.
		 * One of the side effect is to clear some flags about AABB calculation.
		 * like in KX_SoftBodyDeformer.
		 */
		deformer->UpdateBuckets();
	}

	// AABB Box : min/max.
	mt::vec3 aabbMin;
	mt::vec3 aabbMax;

	boundingBox->GetAabb(aabbMin, aabbMax);

	SetBoundsAabb(aabbMin, aabbMax);
}

void KX_GameObject::SetBoundsAabb(const mt::vec3 &aabbMin, const mt::vec3 &aabbMax)
{
	// Set the AABB in culling node box.
	m_cullingNode.GetAabb().Set(aabbMin, aabbMax);

	// Synchronize the AABB with the graphic controller.
	if (m_graphicController) {
		m_graphicController->SetLocalAabb(aabbMin, aabbMax);
	}
}

void KX_GameObject::GetBoundsAabb(mt::vec3 &aabbMin, mt::vec3 &aabbMax) const
{
	// Get the culling node box AABB
	m_cullingNode.GetAabb().Get(aabbMin, aabbMax);
}

SG_CullingNode& KX_GameObject::GetCullingNode()
{
	return m_cullingNode;
}

KX_GameObject::ActivityCullingInfo& KX_GameObject::GetActivityCullingInfo()
{
	return m_activityCullingInfo;
}

void KX_GameObject::SetActivityCullingInfo(const ActivityCullingInfo& cullingInfo)
{
	m_activityCullingInfo = cullingInfo;
}

void KX_GameObject::SetActivityCulling(ActivityCullingInfo::Flag flag, bool enable)
{
	if (enable) {
		m_activityCullingInfo.m_flags = (ActivityCullingInfo::Flag)(m_activityCullingInfo.m_flags | flag);
	}
	else {
		m_activityCullingInfo.m_flags = (ActivityCullingInfo::Flag)(m_activityCullingInfo.m_flags & ~flag);

		// Restore physics or logic when disabling activity culling.
		if (flag & ActivityCullingInfo::ACTIVITY_PHYSICS) {
			RestorePhysics();
		}
		if (flag & ActivityCullingInfo::ACTIVITY_LOGIC) {
			ResumeLogic();
		}
	}
}

void KX_GameObject::SuspendPhysics(bool freeConstraints)
{
	if (m_physicsController) {
		m_physicsController->SuspendPhysics((bool)freeConstraints);
	}
}

void KX_GameObject::RestorePhysics()
{
	if (m_physicsController) {
		m_physicsController->RestorePhysics();
	}
}

void KX_GameObject::UnregisterCollisionCallbacks()
{
	if (!m_physicsController) {
		CM_Warning("trying to unregister collision callbacks for object without collisions: " << GetName());
		return;
	}

	// Unregister from callbacks
	KX_Scene *scene = GetScene();
	PHY_IPhysicsEnvironment *pe = scene->GetPhysicsEnvironment();
	PHY_IPhysicsController *spc = m_physicsController.get();
	// If we are the last to unregister on this physics controller
	if (pe->RemoveCollisionCallback(spc)) {
		// If we are a sensor object
		if (m_clientInfo.isSensor()) {
			// Remove sensor body from physics world
			pe->RemoveSensor(spc);
		}
	}
}

void KX_GameObject::RegisterCollisionCallbacks()
{
	if (!m_physicsController) {
		CM_Warning("trying to register collision callbacks for object without collisions: " << GetName());
		return;
	}

	// Register from callbacks
	KX_Scene *scene = GetScene();
	PHY_IPhysicsEnvironment *pe = scene->GetPhysicsEnvironment();
	PHY_IPhysicsController *spc = m_physicsController.get();
	// If we are the first to register on this physics controller
	if (pe->RequestCollisionCallback(spc)) {
		// If we are a sensor object
		if (m_clientInfo.isSensor()) {
			// Add sensor body to physics world
			pe->AddSensor(spc);
		}
	}
}
void KX_GameObject::RunCollisionCallbacks(KX_GameObject *collider, KX_CollisionContactPointList& contactPointList)
{
#ifdef WITH_PYTHON
	if (!m_collisionCallbacks || PyList_GET_SIZE(m_collisionCallbacks) == 0) {
		return;
	}

	const PHY_ICollData *collData = contactPointList.GetCollData();
	const bool isFirstObject = contactPointList.GetFirstObject();

	PyObject *args[] = {collider->GetProxy(),
		                PyObjectFrom(collData->GetWorldPoint(0, isFirstObject)),
		                PyObjectFrom(collData->GetNormal(0, isFirstObject)),
		                contactPointList.GetProxy()};
	EXP_RunPythonCallBackList(m_collisionCallbacks, args, 1, ARRAY_SIZE(args));

	for (unsigned int i = 0; i < ARRAY_SIZE(args); ++i) {
		Py_DECREF(args[i]);
	}

	// Invalidate the collison contact point to avoid access to it in next frame
	contactPointList.InvalidateProxy();
#endif
}

template <bool recursive>
static void walk_children(const SG_Node *node, std::vector<KX_GameObject *>& list)
{
	if (!node) {
		return;
	}

	const NodeList& children = node->GetChildren();

	for (SG_Node *childnode : children) {
		KX_GameObject *childobj = static_cast<KX_GameObject *>(childnode->GetClientObject());
		if (childobj) {
			list.push_back(childobj);
		}

		/* If the childobj is nullptr then this may be an inverse parent link
		 * so a non recursive search should still look down this node. */
		if (recursive || !childobj) {
			walk_children<recursive>(childnode, list);
		}
	}
}

std::vector<KX_GameObject *> KX_GameObject::GetChildren() const
{
	std::vector<KX_GameObject *> list;
	walk_children<false>(m_sgNode.get(), list);
	return list;
}

std::vector<KX_GameObject *> KX_GameObject::GetChildrenRecursive() const
{
	std::vector<KX_GameObject *> list;
	walk_children<true>(m_sgNode.get(), list);
	return list;
}

EXP_ListValue<KX_PythonComponent> *KX_GameObject::GetComponents() const
{
	return m_components.get();
}

void KX_GameObject::SetComponents(EXP_ListValue<KX_PythonComponent> *components)
{
	m_components.reset(components);
}

void KX_GameObject::UpdateComponents()
{
#ifdef WITH_PYTHON
	if (!m_components) {
		return;
	}

	for (KX_PythonComponent *comp : m_components) {
		comp->Update();
	}

#endif // WITH_PYTHON
}

KX_Scene *KX_GameObject::GetScene()
{
	BLI_assert(m_sgNode);
	return static_cast<KX_Scene *>(m_sgNode->GetClientInfo());
}

#ifdef WITH_PYTHON

#define PYTHON_CHECK_PHYSICS_CONTROLLER(obj, attr, ret) \
	if (!(obj)->GetPhysicsController()) { \
		PyErr_Format(PyExc_AttributeError, "KX_GameObject.%s, is missing a physics controller", (attr)); \
		return (ret); \
	}

PyMethodDef KX_GameObject::Methods[] = {
	{"applyForce", (PyCFunction)KX_GameObject::sPyApplyForce, METH_VARARGS},
	{"applyTorque", (PyCFunction)KX_GameObject::sPyApplyTorque, METH_VARARGS},
	{"applyRotation", (PyCFunction)KX_GameObject::sPyApplyRotation, METH_VARARGS},
	{"applyMovement", (PyCFunction)KX_GameObject::sPyApplyMovement, METH_VARARGS},
	{"getLinearVelocity", (PyCFunction)KX_GameObject::sPyGetLinearVelocity, METH_VARARGS},
	{"setLinearVelocity", (PyCFunction)KX_GameObject::sPySetLinearVelocity, METH_VARARGS},
	{"getAngularVelocity", (PyCFunction)KX_GameObject::sPyGetAngularVelocity, METH_VARARGS},
	{"setAngularVelocity", (PyCFunction)KX_GameObject::sPySetAngularVelocity, METH_VARARGS},
	{"getVelocity", (PyCFunction)KX_GameObject::sPyGetVelocity, METH_VARARGS},
	{"setDamping", (PyCFunction)KX_GameObject::sPySetDamping, METH_VARARGS},
	{"getReactionForce", (PyCFunction)KX_GameObject::sPyGetReactionForce, METH_NOARGS},
	{"alignAxisToVect", (PyCFunction)KX_GameObject::sPyAlignAxisToVect, METH_VARARGS | METH_KEYWORDS},
	{"getAxisVect", (PyCFunction)KX_GameObject::sPyGetAxisVect, METH_O},
	{"suspendPhysics", (PyCFunction)KX_GameObject::sPySuspendPhysics, METH_VARARGS},
	{"restorePhysics", (PyCFunction)KX_GameObject::sPyRestorePhysics, METH_NOARGS},
	{"suspendDynamics", (PyCFunction)KX_GameObject::sPySuspendDynamics, METH_VARARGS},
	{"restoreDynamics", (PyCFunction)KX_GameObject::sPyRestoreDynamics, METH_NOARGS},
	{"enableRigidBody", (PyCFunction)KX_GameObject::sPyEnableRigidBody, METH_NOARGS},
	{"disableRigidBody", (PyCFunction)KX_GameObject::sPyDisableRigidBody, METH_NOARGS},
	{"applyImpulse", (PyCFunction)KX_GameObject::sPyApplyImpulse, METH_VARARGS},
	{"setCollisionMargin", (PyCFunction)KX_GameObject::sPySetCollisionMargin, METH_O},
	{"collide", (PyCFunction)KX_GameObject::sPyCollide, METH_O},
	{"setParent", (PyCFunction)KX_GameObject::sPySetParent, METH_VARARGS | METH_KEYWORDS},
	{"setVisible", (PyCFunction)KX_GameObject::sPySetVisible, METH_VARARGS},
	{"setOcclusion", (PyCFunction)KX_GameObject::sPySetOcclusion, METH_VARARGS},
	{"removeParent", (PyCFunction)KX_GameObject::sPyRemoveParent, METH_NOARGS},


	{"getPhysicsId", (PyCFunction)KX_GameObject::sPyGetPhysicsId, METH_NOARGS},
	{"getPropertyNames", (PyCFunction)KX_GameObject::sPyGetPropertyNames, METH_NOARGS},
	{"replaceMesh", (PyCFunction)KX_GameObject::sPyReplaceMesh, METH_VARARGS | METH_KEYWORDS},
	{"endObject", (PyCFunction)KX_GameObject::sPyEndObject, METH_NOARGS},
	{"reinstancePhysicsMesh", (PyCFunction)KX_GameObject::sPyReinstancePhysicsMesh, METH_VARARGS | METH_KEYWORDS},
	{"replacePhysicsShape", (PyCFunction)KX_GameObject::sPyReplacePhysicsShape, METH_O},

	EXP_PYMETHODTABLE_KEYWORDS(KX_GameObject, rayCastTo),
	EXP_PYMETHODTABLE_KEYWORDS(KX_GameObject, rayCast),
	EXP_PYMETHODTABLE_O(KX_GameObject, getDistanceTo),
	EXP_PYMETHODTABLE_O(KX_GameObject, getVectTo),
	EXP_PYMETHODTABLE_KEYWORDS(KX_GameObject, sendMessage),
	EXP_PYMETHODTABLE(KX_GameObject, addDebugProperty),

	EXP_PYMETHODTABLE_KEYWORDS(KX_GameObject, playAction),
	EXP_PYMETHODTABLE(KX_GameObject, stopAction),
	EXP_PYMETHODTABLE(KX_GameObject, getActionFrame),
	EXP_PYMETHODTABLE(KX_GameObject, getActionName),
	EXP_PYMETHODTABLE(KX_GameObject, setActionFrame),
	EXP_PYMETHODTABLE(KX_GameObject, isPlayingAction),

	// dict style access for props
	{"get", (PyCFunction)KX_GameObject::sPyget, METH_VARARGS},

	{nullptr, nullptr} //Sentinel
};

#define MAX_LAYERS ((1 << 20) - 1)
#define MAX_MASK ((1 << OB_MAX_COL_MASKS) - 1)

EXP_Attribute KX_GameObject::Attributes[] = {
	EXP_ATTRIBUTE_RO("currentLodLevel", m_currentLodLevel),
	EXP_ATTRIBUTE_RW_FUNCTION("lodManager", pyattr_get_lodManager, pyattr_set_lodManager),
	EXP_ATTRIBUTE_RW_FUNCTION("name", pyattr_get_name, pyattr_set_name),
	EXP_ATTRIBUTE_RO_FUNCTION("parent", pyattr_get_parent),
	EXP_ATTRIBUTE_RO("groupMembers", m_instanceObjects),
	EXP_ATTRIBUTE_RO("groupObject", m_dupliGroupObject),
	EXP_ATTRIBUTE_RO_FUNCTION("scene", pyattr_get_scene),
	EXP_ATTRIBUTE_RO_FUNCTION("life", pyattr_get_life),
	EXP_ATTRIBUTE_RW_FUNCTION("mass", pyattr_get_mass, pyattr_set_mass),
	EXP_ATTRIBUTE_RO_FUNCTION("isSuspendDynamics", pyattr_get_is_suspend_dynamics),
	EXP_ATTRIBUTE_RW_FUNCTION_CHECK_RANGE("linVelocityMin", pyattr_get_lin_vel_min, pyattr_set_lin_vel_min, pyattr_check_physics, 0.0f, FLT_MAX, false),
	EXP_ATTRIBUTE_RW_FUNCTION_CHECK_RANGE("linVelocityMax", pyattr_get_lin_vel_max, pyattr_set_lin_vel_max, pyattr_check_physics, 0.0f, FLT_MAX, false),
	EXP_ATTRIBUTE_RW_FUNCTION_CHECK_RANGE("angularVelocityMin", pyattr_get_ang_vel_min, pyattr_set_ang_vel_min, pyattr_check_physics, 0.0f, FLT_MAX, false),
	EXP_ATTRIBUTE_RW_FUNCTION_CHECK_RANGE("angularVelocityMax", pyattr_get_ang_vel_max, pyattr_set_ang_vel_max, pyattr_check_physics, 0.0f, FLT_MAX, false),
	EXP_ATTRIBUTE_RW_FUNCTION_RANGE("layer", pyattr_get_layer, pyattr_set_layer, 1, MAX_LAYERS, false),
	EXP_ATTRIBUTE_RW_FUNCTION("visible",  pyattr_get_visible, pyattr_set_visible),
	EXP_ATTRIBUTE_RO_FUNCTION("culled", pyattr_get_culled),
	EXP_ATTRIBUTE_RO_FUNCTION("cullingBox", pyattr_get_cullingBox),
	EXP_ATTRIBUTE_RW("occlusion", m_bOccluder),
	EXP_ATTRIBUTE_RW_FUNCTION_RANGE("physicsCullingRadius", pyattr_get_physicsCullingRadius, pyattr_set_physicsCullingRadius, 0.0f, FLT_MAX, false),
	EXP_ATTRIBUTE_RW_FUNCTION_RANGE("logicCullingRadius", pyattr_get_logicCullingRadius, pyattr_set_logicCullingRadius, 0.0f, FLT_MAX, false),
	EXP_ATTRIBUTE_RW_FUNCTION("physicsCulling", pyattr_get_physicsCulling, pyattr_set_physicsCulling),
	EXP_ATTRIBUTE_RW_FUNCTION("logicCulling", pyattr_get_logicCulling, pyattr_set_logicCulling),
	EXP_ATTRIBUTE_RW_FUNCTION("position", pyattr_get_worldPosition, pyattr_set_localPosition),
	EXP_ATTRIBUTE_RO_FUNCTION("localInertia", pyattr_get_localInertia),
	EXP_ATTRIBUTE_RW_FUNCTION("orientation", pyattr_get_worldOrientation, pyattr_set_localOrientation),
	EXP_ATTRIBUTE_RW_FUNCTION("scaling", pyattr_get_worldScaling, pyattr_set_localScaling),
	EXP_ATTRIBUTE_RW_FUNCTION_RANGE("timeOffset", pyattr_get_timeOffset, pyattr_set_timeOffset, 0.0f, FLT_MAX, false),
	EXP_ATTRIBUTE_RW_FUNCTION_CHECK("collisionCallbacks", pyattr_get_collisionCallbacks, pyattr_set_collisionCallbacks, pyattr_check_physics),
	EXP_ATTRIBUTE_RW_FUNCTION_RANGE("collisionGroup", pyattr_get_collisionGroup, pyattr_set_collisionGroup, 1, MAX_MASK, false),
	EXP_ATTRIBUTE_RW_FUNCTION_RANGE("collisionMask", pyattr_get_collisionMask, pyattr_set_collisionMask, 1, MAX_MASK, false),
	EXP_ATTRIBUTE_RO("meshes", m_meshes),
	EXP_ATTRIBUTE_RO_FUNCTION("batchGroup", pyattr_get_batchGroup),
	EXP_ATTRIBUTE_RW_FUNCTION("localOrientation", pyattr_get_localOrientation, pyattr_set_localOrientation),
	EXP_ATTRIBUTE_RW_FUNCTION("worldOrientation", pyattr_get_worldOrientation, pyattr_set_worldOrientation),
	EXP_ATTRIBUTE_RW_FUNCTION("localPosition", pyattr_get_localPosition, pyattr_set_localPosition),
	EXP_ATTRIBUTE_RW_FUNCTION("worldPosition", pyattr_get_worldPosition, pyattr_set_worldPosition),
	EXP_ATTRIBUTE_RW_FUNCTION("localScale", pyattr_get_localScaling, pyattr_set_localScaling),
	EXP_ATTRIBUTE_RW_FUNCTION("worldScale", pyattr_get_worldScaling, pyattr_set_worldScaling),
	EXP_ATTRIBUTE_RW_FUNCTION("localTransform", pyattr_get_localTransform, pyattr_set_localTransform),
	EXP_ATTRIBUTE_RW_FUNCTION("worldTransform", pyattr_get_worldTransform, pyattr_set_worldTransform),
	EXP_ATTRIBUTE_RW_FUNCTION_CHECK("linearVelocity", pyattr_get_localLinearVelocity, pyattr_set_worldLinearVelocity, pyattr_check_physics),
	EXP_ATTRIBUTE_RW_FUNCTION_CHECK("localLinearVelocity", pyattr_get_localLinearVelocity, pyattr_set_localLinearVelocity, pyattr_check_physics),
	EXP_ATTRIBUTE_RW_FUNCTION_CHECK("worldLinearVelocity", pyattr_get_worldLinearVelocity, pyattr_set_worldLinearVelocity, pyattr_check_physics),
	EXP_ATTRIBUTE_RW_FUNCTION_CHECK("angularVelocity", pyattr_get_localAngularVelocity, pyattr_set_worldAngularVelocity, pyattr_check_physics),
	EXP_ATTRIBUTE_RW_FUNCTION_CHECK("localAngularVelocity", pyattr_get_localAngularVelocity, pyattr_set_localAngularVelocity, pyattr_check_physics),
	EXP_ATTRIBUTE_RW_FUNCTION_CHECK("worldAngularVelocity", pyattr_get_worldAngularVelocity, pyattr_set_worldAngularVelocity, pyattr_check_physics),
	EXP_ATTRIBUTE_RW_FUNCTION_CHECK("linearDamping", pyattr_get_linearDamping, pyattr_set_linearDamping, pyattr_check_physics),
	EXP_ATTRIBUTE_RW_FUNCTION_CHECK("angularDamping", pyattr_get_angularDamping, pyattr_set_angularDamping, pyattr_check_physics),
	EXP_ATTRIBUTE_RO_FUNCTION("children", pyattr_get_children),
	EXP_ATTRIBUTE_RO_FUNCTION("childrenRecursive", pyattr_get_children_recursive),
	EXP_ATTRIBUTE_RW_FUNCTION("color", pyattr_get_obcolor, pyattr_set_obcolor),
	EXP_ATTRIBUTE_RW_FUNCTION("debug", pyattr_get_debug, pyattr_set_debug),
	EXP_ATTRIBUTE_RO("components", m_components),
	EXP_ATTRIBUTE_RW_FUNCTION("debugRecursive", pyattr_get_debugRecursive, pyattr_set_debugRecursive),
	EXP_ATTRIBUTE_RW_FUNCTION("gravity", pyattr_get_gravity, pyattr_set_gravity),

	EXP_ATTRIBUTE_NULL //Sentinel
};

#undef MAX_MASK
#undef MAX_LAYERS

PyObject *KX_GameObject::PyReplaceMesh(PyObject *args, PyObject *kwds)
{
	PyObject *value;
	int use_gfx = 1, use_phys = 0;
	KX_Mesh *new_mesh;

	if (!EXP_ParseTupleArgsAndKeywords(args, kwds, "O|ii:replaceMesh",
	                                   {"mesh", "useDisplayMesh", "usePhysicsMesh", 0}, &value, &use_gfx, &use_phys)) {
		return nullptr;
	}

	if (!ConvertPythonToMesh(GetScene(), value, &new_mesh, false, "gameOb.replaceMesh(value): KX_GameObject")) {
		return nullptr;
	}

	ReplaceMesh(new_mesh, (bool)use_gfx, (bool)use_phys);
	Py_RETURN_NONE;
}

PyObject *KX_GameObject::PyEndObject()
{
	GetScene()->DelayedRemoveObject(this);

	Py_RETURN_NONE;
}

PyObject *KX_GameObject::PyReinstancePhysicsMesh(PyObject *args, PyObject *kwds)
{
	KX_GameObject *gameobj = nullptr;
	KX_Mesh *mesh = nullptr;
	int dupli = 0;

	PyObject *gameobj_py = nullptr;
	PyObject *mesh_py = nullptr;

	KX_Scene *scene = GetScene();

	if (!EXP_ParseTupleArgsAndKeywords(args, kwds, "|OOi:reinstancePhysicsMesh",
	                                   {"gameObject", "meshObject", "dupli", 0}, &gameobj_py, &mesh_py, &dupli) ||
	    (gameobj_py && !ConvertPythonToGameObject(scene, gameobj_py, &gameobj, true, "gameOb.reinstancePhysicsMesh(obj, mesh, dupli): KX_GameObject")) ||
	    (mesh_py && !ConvertPythonToMesh(scene, mesh_py, &mesh, true, "gameOb.reinstancePhysicsMesh(obj, mesh, dupli): KX_GameObject"))) {
		return nullptr;
	}

	/* gameobj and mesh can be nullptr */
	if (m_physicsController && m_physicsController->ReinstancePhysicsShape(gameobj, mesh, dupli)) {
		Py_RETURN_TRUE;
	}

	Py_RETURN_FALSE;
}

PyObject *KX_GameObject::PyReplacePhysicsShape(PyObject *value)
{
	KX_GameObject *gameobj;

	if (!ConvertPythonToGameObject(GetScene(), value, &gameobj, false, "gameOb.replacePhysicsShape(obj): KX_GameObject")) {
		return nullptr;
	}

	if (!m_physicsController || !gameobj->GetPhysicsController()) {
		PyErr_SetString(PyExc_AttributeError, "gameOb.replacePhysicsShape(obj): function only available for objects with collisions enabled");
		return nullptr;
	}

	m_physicsController->ReplacePhysicsShape(gameobj->GetPhysicsController());
	Py_RETURN_NONE;
}

static PyObject *Map_GetItem(PyObject *self_v, PyObject *item)
{
	KX_GameObject *self = static_cast<KX_GameObject *>EXP_PROXY_REF(self_v);

	if (!self) {
		PyErr_SetString(PyExc_SystemError, "value = gameOb[key]: KX_GameObject, " EXP_PROXY_ERROR_MSG);
		return nullptr;
	}

	const char *attr_str = _PyUnicode_AsString(item);
	if (!attr_str) {
		PyErr_SetString(PyExc_KeyError, "value = gameOb[key]: KX_GameObject, key must be a string");
		return nullptr;
	}

	EXP_PropValue *prop = self->GetProperty(attr_str);

	if (!prop) {
		PyErr_Format(PyExc_KeyError, "value = gameOb[key]: KX_GameObject, key \"%s\" does not exist", attr_str);
		return nullptr;
	}

	return prop->ConvertValueToPython();
}


static int Map_SetItem(PyObject *self_v, PyObject *key, PyObject *val)
{
	KX_GameObject *self = static_cast<KX_GameObject *>EXP_PROXY_REF(self_v);

	if (!self) {
		PyErr_SetString(PyExc_SystemError, "gameOb[key] = value: KX_GameObject, " EXP_PROXY_ERROR_MSG);
		return -1;
	}

	const char *attr_str = _PyUnicode_AsString(key);
	if (!attr_str) {
		PyErr_SetString(PyExc_KeyError, "gameOb[key] = value: KX_GameObject, key must be a string");
		return -1;
	}

	// del ob["key"]
	if (!val) {
		if (!self->RemoveProperty(attr_str)) {
			PyErr_Format(PyExc_KeyError, "gameOb[key] = value: KX_GameObject, key \"%s\" does not exist", attr_str);
			return -1;
		}
	}
	// ob["key"] = value
	else {
		EXP_PropValue *vallie = EXP_PropValue::ConvertPythonToValue(val);
		self->SetProperty(attr_str, vallie);
	}

	return 0;
}

static int Seq_Contains(PyObject *self_v, PyObject *value)
{
	KX_GameObject *self = static_cast<KX_GameObject *>EXP_PROXY_REF(self_v);

	if (self == nullptr) {
		PyErr_SetString(PyExc_SystemError, "val in gameOb: KX_GameObject, " EXP_PROXY_ERROR_MSG);
		return -1;
	}

	const char *attr_str = _PyUnicode_AsString(value);
	if (!attr_str) {
		PyErr_SetString(PyExc_KeyError, "val in gameOb: KX_GameObject, key must be a string");
		return -1;
	}

	if (self->GetProperty(attr_str)) {
		return 1;
	}

	return 0;
}


PyMappingMethods KX_GameObject::Mapping = {
	(lenfunc)nullptr,                               /*inquiry mp_length */
	(binaryfunc)Map_GetItem,        /*binaryfunc mp_subscript */
	(objobjargproc)Map_SetItem, /*objobjargproc mp_ass_subscript */
};

PySequenceMethods KX_GameObject::Sequence = {
	nullptr,        /* Cant set the len otherwise it can evaluate as false */
	nullptr,        /* sq_concat */
	nullptr,        /* sq_repeat */
	nullptr,        /* sq_item */
	nullptr,        /* sq_slice */
	nullptr,        /* sq_ass_item */
	nullptr,        /* sq_ass_slice */
	(objobjproc)Seq_Contains,   /* sq_contains */
	(binaryfunc)nullptr,  /* sq_inplace_concat */
	(ssizeargfunc)nullptr,  /* sq_inplace_repeat */
};

PyTypeObject KX_GameObject::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"KX_GameObject",
	sizeof(EXP_PyObjectPlus_Proxy),
	0,
	py_base_dealloc,
	0,
	0,
	0,
	0,
	py_base_repr,
	0,
	&Sequence,
	&Mapping,
	0, 0, 0,
	nullptr,
	nullptr,
	0,
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	0, 0, 0, 0, 0, 0, 0,
	Methods,
	0,
	0,
	&EXP_Value::Type,
	0, 0, 0, 0, 0, 0,
	py_base_new
};

bool KX_GameObject::pyattr_check_physics(const EXP_Attribute *attrdef)
{
	if (!m_physicsController) {
		attrdef->PrintError(": KX_GameObject is missing a physics controller");
		return false;
	}

	return true;
}

std::string KX_GameObject::pyattr_get_name()
{
	return m_name;
}

bool KX_GameObject::pyattr_set_name(const std::string& value)
{
	KX_Scene *scene = GetScene();
	if (!scene->GetResources().ChangeObjectName(m_name, value, this)) {
		PyErr_Format(PyExc_TypeError, "gameOb.name = str: name %s is already used by an other non-replica game object", m_name.c_str());
		return false;
	}

	// Change the name
	m_name = value;
	return true;
}

KX_GameObject *KX_GameObject::pyattr_get_parent()
{
	return GetParent();
}

PyObject *KX_GameObject::pyattr_get_collisionCallbacks()
{
	// Only objects with a physics controller should have collision callbacks

	// Return the existing callbacks
	if (!m_collisionCallbacks) {
		m_collisionCallbacks = PyList_New(0);
		// Subscribe to collision update from KX_CollisionEventManager
		RegisterCollisionCallbacks();
	}
	Py_INCREF(m_collisionCallbacks);
	return m_collisionCallbacks;
}

bool KX_GameObject::pyattr_set_collisionCallbacks(PyObject *value, const EXP_Attribute *attrdef)
{
	// Only objects with a physics controller should have collision callbacks

	if (!PyList_CheckExact(value)) {
		attrdef->PrintError(" = list: Expected a list.");
		return false;
	}

	if (!m_collisionCallbacks) {
		RegisterCollisionCallbacks();
	}
	else {
		Py_DECREF(m_collisionCallbacks);
	}

	Py_INCREF(value);
	m_collisionCallbacks = value;

	return true;
}

int KX_GameObject::pyattr_get_collisionGroup()
{
	return GetCollisionGroup();
}

void KX_GameObject::pyattr_set_collisionGroup(int value)
{
	SetCollisionGroup(value);
}

int KX_GameObject::pyattr_get_collisionMask()
{
	return GetCollisionMask();
}

void KX_GameObject::pyattr_set_collisionMask(int value)
{
	SetCollisionMask(value);
}

KX_Scene *KX_GameObject::pyattr_get_scene()
{
	return GetScene();
}

float KX_GameObject::pyattr_get_life()
{
	EXP_PropValue *life = GetProperty("::timebomb");
	if (life) {
		// this convert the timebomb seconds to frames, hard coded 50.0f (assuming 50fps)
		// value hardcoded in KX_Scene::AddReplicaObject()
		return static_cast<EXP_PropFloat *>(life)->GetValue() * 50.0;
	}
	else {
		return 0.0f;
	}
}

float KX_GameObject::pyattr_get_mass()
{
	return m_physicsController ? m_physicsController->GetMass() : 0.0f;
}

void KX_GameObject::pyattr_set_mass(float value)
{
	if (m_physicsController) {
		m_physicsController->SetMass(value);
	}
}

bool KX_GameObject::pyattr_get_is_suspend_dynamics()
{
	return IsDynamicsSuspended();
}

float KX_GameObject::pyattr_get_lin_vel_min()
{
	return m_physicsController->GetLinVelocityMin();
}

void KX_GameObject::pyattr_set_lin_vel_min(float value)
{
	m_physicsController->SetLinVelocityMin(value);
}

float KX_GameObject::pyattr_get_lin_vel_max()
{
	return m_physicsController->GetLinVelocityMax();
}

void KX_GameObject::pyattr_set_lin_vel_max(float value)
{
	m_physicsController->SetLinVelocityMax(value);
}

float KX_GameObject::pyattr_get_ang_vel_min()
{
	return m_physicsController->GetAngularVelocityMin();
}

void KX_GameObject::pyattr_set_ang_vel_min(float value)
{
	m_physicsController->SetAngularVelocityMin(value);
}

float KX_GameObject::pyattr_get_ang_vel_max()
{
	return m_physicsController->GetAngularVelocityMax();
}

void KX_GameObject::pyattr_set_ang_vel_max(float value)
{
	m_physicsController->SetAngularVelocityMax(value);
}

int KX_GameObject::pyattr_get_layer()
{
	return m_layer;
}

void KX_GameObject::pyattr_set_layer(int value)
{
	// SetLayer function is overrided in KX_LightObject.
	SetLayer(value);
}

bool KX_GameObject::pyattr_get_visible()
{
	return m_bVisible;
}

void KX_GameObject::pyattr_set_visible(bool value)
{
	SetVisible(value, false);
}

bool KX_GameObject::pyattr_get_culled()
{
	return m_cullingNode.GetCulled();
}

EXP_ValuePythonOwn KX_GameObject::pyattr_get_cullingBox()
{
	return (new KX_BoundingBox(this));
}

bool KX_GameObject::pyattr_get_physicsCulling()
{
	return m_activityCullingInfo.m_flags & ActivityCullingInfo::ACTIVITY_PHYSICS;
}

void KX_GameObject::pyattr_set_physicsCulling(bool value)
{
	SetActivityCulling(ActivityCullingInfo::ACTIVITY_PHYSICS, value);
}

bool KX_GameObject::pyattr_get_logicCulling()
{
	return m_activityCullingInfo.m_flags & ActivityCullingInfo::ACTIVITY_LOGIC;
}

void KX_GameObject::pyattr_set_logicCulling(bool value)
{
	SetActivityCulling(ActivityCullingInfo::ACTIVITY_LOGIC, value);
}

float KX_GameObject::pyattr_get_physicsCullingRadius()
{
	return std::sqrt(m_activityCullingInfo.m_physicsRadius);
}

void KX_GameObject::pyattr_set_physicsCullingRadius(float value)
{
	m_activityCullingInfo.m_physicsRadius = value * value;
}

float KX_GameObject::pyattr_get_logicCullingRadius()
{
	return std::sqrt(m_activityCullingInfo.m_logicRadius);
}

void KX_GameObject::pyattr_set_logicCullingRadius(float value)
{
	m_activityCullingInfo.m_logicRadius = value * value;
}

mt::vec3 KX_GameObject::pyattr_get_worldPosition()
{
	return NodeGetWorldPosition();
}

void KX_GameObject::pyattr_set_worldPosition(const mt::vec3& value)
{
	NodeSetWorldPosition(value);
	NodeUpdate();
}

mt::vec3 KX_GameObject::pyattr_get_localPosition()
{
	return NodeGetLocalPosition();
}

void KX_GameObject::pyattr_set_localPosition(const mt::vec3& value)
{
	NodeSetLocalPosition(value);
	NodeUpdate();
}

mt::vec3 KX_GameObject::pyattr_get_localInertia()
{
	return m_physicsController->GetLocalInertia();
}

mt::mat3 KX_GameObject::pyattr_get_worldOrientation()
{
	return NodeGetWorldOrientation();
}

void KX_GameObject::pyattr_set_worldOrientation(const mt::mat3& value)
{
	NodeSetGlobalOrientation(value);
	NodeUpdate();
}

mt::mat3 KX_GameObject::pyattr_get_localOrientation()
{
	return NodeGetLocalOrientation();
}

void KX_GameObject::pyattr_set_localOrientation(const mt::mat3& value)
{
	NodeSetLocalOrientation(value);
	NodeUpdate();
}

mt::vec3 KX_GameObject::pyattr_get_worldScaling()
{
	return NodeGetWorldScaling();
}

void KX_GameObject::pyattr_set_worldScaling(const mt::vec3& value)
{
	NodeSetWorldScale(value);
	NodeUpdate();
}

mt::vec3 KX_GameObject::pyattr_get_localScaling()
{
	return NodeGetLocalScaling();
}

void KX_GameObject::pyattr_set_localScaling(const mt::vec3& value)
{
	NodeSetLocalScale(value);
	NodeUpdate();
}

mt::mat4 KX_GameObject::pyattr_get_localTransform()
{
	return mt::mat4::FromAffineTransform(NodeGetLocalTransform());
}

void KX_GameObject::pyattr_set_localTransform(const mt::mat4& value)
{
	NodeSetLocalPosition(value.TranslationVector3D());
	NodeSetLocalOrientation(value.RotationMatrix());
	NodeSetLocalScale(value.ScaleVector3D());
}

mt::mat4 KX_GameObject::pyattr_get_worldTransform()
{
	return mt::mat4::FromAffineTransform(NodeGetWorldTransform());
}

void KX_GameObject::pyattr_set_worldTransform(const mt::mat4& value)
{
	NodeSetWorldPosition(value.TranslationVector3D());
	NodeSetGlobalOrientation(value.RotationMatrix());
	NodeSetWorldScale(value.ScaleVector3D());
}

mt::vec3 KX_GameObject::pyattr_get_worldLinearVelocity()
{
	return GetLinearVelocity(false);
}

void KX_GameObject::pyattr_set_worldLinearVelocity(const mt::vec3& value)
{
	SetLinearVelocity(value, false);
}

mt::vec3 KX_GameObject::pyattr_get_localLinearVelocity()
{
	return GetLinearVelocity(true);
}

void KX_GameObject::pyattr_set_localLinearVelocity(const mt::vec3& value)
{
	SetLinearVelocity(value, true);
}

mt::vec3 KX_GameObject::pyattr_get_worldAngularVelocity()
{
	return GetAngularVelocity(false);
}

void KX_GameObject::pyattr_set_worldAngularVelocity(const mt::vec3& value)
{
	SetAngularVelocity(value, false);
}

mt::vec3 KX_GameObject::pyattr_get_localAngularVelocity()
{
	return GetAngularVelocity(true);
}

void KX_GameObject::pyattr_set_localAngularVelocity(const mt::vec3& value)
{
	SetAngularVelocity(value, true);
}

mt::vec3 KX_GameObject::pyattr_get_gravity()
{
	return GetGravity();
}

void KX_GameObject::pyattr_set_gravity(const mt::vec3& value)
{
	SetGravity(value);
}

float KX_GameObject::pyattr_get_linearDamping()
{
	return GetLinearDamping();
}

void KX_GameObject::pyattr_set_linearDamping(float value)
{
	SetLinearDamping(value);
}

float KX_GameObject::pyattr_get_angularDamping()
{
	return GetAngularDamping();
}

void KX_GameObject::pyattr_set_angularDamping(float value)
{
	SetAngularDamping(value);
}

float KX_GameObject::pyattr_get_timeOffset()
{
	SG_Node *sg_parent = m_sgNode->GetParent();
	if (sg_parent && sg_parent->IsSlowParent()) {
		return static_cast<KX_SlowParentRelation *>(sg_parent->GetParentRelation())->GetTimeOffset();
	}
	else {
		return 0.0f;
	}
}

void KX_GameObject::pyattr_set_timeOffset(float value)
{
	SG_Node *sg_parent = m_sgNode->GetParent();
	if (sg_parent && sg_parent->IsSlowParent()) {
		static_cast<KX_SlowParentRelation *>(sg_parent->GetParentRelation())->SetTimeOffset(value);
	}
}

KX_BatchGroup *KX_GameObject::pyattr_get_batchGroup()
{
	if (!m_meshUser) {
		return nullptr;
	}

	return static_cast<KX_BatchGroup *>(m_meshUser->GetBatchGroup());
}

mt::vec4 KX_GameObject::pyattr_get_obcolor()
{
	return m_objectColor;
}

void KX_GameObject::pyattr_set_obcolor(const mt::vec4& value)
{
	m_objectColor = value;
}

EXP_ValuePythonOwn KX_GameObject::pyattr_get_children()
{
	return (new EXP_ListValue<KX_GameObject>(GetChildren()));
}

EXP_ValuePythonOwn KX_GameObject::pyattr_get_children_recursive()
{
	return (new EXP_ListValue<KX_GameObject>(GetChildrenRecursive()));
}

bool KX_GameObject::pyattr_get_debug()
{
	return GetScene()->ObjectInDebugList(this);
}

void KX_GameObject::pyattr_set_debug(bool value)
{
	SetUseDebugProperties(value, false);
}

bool KX_GameObject::pyattr_get_debugRecursive()
{
	return GetScene()->ObjectInDebugList(this);
}

void KX_GameObject::pyattr_set_debugRecursive(bool value)
{
	SetUseDebugProperties(value, true);
}

KX_LodManager *KX_GameObject::pyattr_get_lodManager()
{
	return m_lodManager;
}

bool KX_GameObject::pyattr_set_lodManager(PyObject *value) // TODO strict auto check.
{
	KX_LodManager *lodManager = nullptr;
	if (!ConvertPythonToLodManager(value, &lodManager, true, "gameobj.lodManager: KX_GameObject")) {
		return false;
	}

	SetLodManager(lodManager);

	return true;
}

PyObject *KX_GameObject::PyApplyForce(PyObject *args)
{
	int local = 0;
	PyObject *pyvect;

	if (PyArg_ParseTuple(args, "O|i:applyForce", &pyvect, &local)) {
		mt::vec3 force;
		if (PyVecTo(pyvect, force)) {
			ApplyForce(force, (local != 0));
			Py_RETURN_NONE;
		}
	}
	return nullptr;
}

PyObject *KX_GameObject::PyApplyTorque(PyObject *args)
{
	int local = 0;
	PyObject *pyvect;

	if (PyArg_ParseTuple(args, "O|i:applyTorque", &pyvect, &local)) {
		mt::vec3 torque;
		if (PyVecTo(pyvect, torque)) {
			ApplyTorque(torque, (local != 0));
			Py_RETURN_NONE;
		}
	}
	return nullptr;
}

PyObject *KX_GameObject::PyApplyRotation(PyObject *args)
{
	int local = 0;
	PyObject *pyvect;

	if (PyArg_ParseTuple(args, "O|i:applyRotation", &pyvect, &local)) {
		mt::vec3 rotation;
		if (PyVecTo(pyvect, rotation)) {
			ApplyRotation(rotation, (local != 0));
			Py_RETURN_NONE;
		}
	}
	return nullptr;
}

PyObject *KX_GameObject::PyApplyMovement(PyObject *args)
{
	int local = 0;
	PyObject *pyvect;

	if (PyArg_ParseTuple(args, "O|i:applyMovement", &pyvect, &local)) {
		mt::vec3 movement;
		if (PyVecTo(pyvect, movement)) {
			ApplyMovement(movement, (local != 0));
			Py_RETURN_NONE;
		}
	}
	return nullptr;
}

PyObject *KX_GameObject::PyGetLinearVelocity(PyObject *args)
{
	// only can get the velocity if we have a physics object connected to us...
	int local = 0;

	if (PyArg_ParseTuple(args, "|i:getLinearVelocity", &local)) {
		return PyObjectFrom(GetLinearVelocity((local != 0)));
	}
	else {
		return nullptr;
	}
}

PyObject *KX_GameObject::PySetLinearVelocity(PyObject *args)
{
	int local = 0;
	PyObject *pyvect;

	if (PyArg_ParseTuple(args, "O|i:setLinearVelocity", &pyvect, &local)) {
		mt::vec3 velocity;
		if (PyVecTo(pyvect, velocity)) {
			SetLinearVelocity(velocity, (local != 0));
			Py_RETURN_NONE;
		}
	}
	return nullptr;
}

PyObject *KX_GameObject::PyGetAngularVelocity(PyObject *args)
{
	// only can get the velocity if we have a physics object connected to us...
	int local = 0;

	if (PyArg_ParseTuple(args, "|i:getAngularVelocity", &local)) {
		return PyObjectFrom(GetAngularVelocity((local != 0)));
	}
	else {
		return nullptr;
	}
}

PyObject *KX_GameObject::PySetAngularVelocity(PyObject *args)
{
	int local = 0;
	PyObject *pyvect;

	if (PyArg_ParseTuple(args, "O|i:setAngularVelocity", &pyvect, &local)) {
		mt::vec3 velocity;
		if (PyVecTo(pyvect, velocity)) {
			SetAngularVelocity(velocity, (local != 0));
			Py_RETURN_NONE;
		}
	}
	return nullptr;
}

PyObject *KX_GameObject::PySetDamping(PyObject *args)
{
	float linear;
	float angular;

	if (!PyArg_ParseTuple(args, "ff:setDamping", &linear, &angular)) {
		return nullptr;
	}

	SetDamping(linear, angular);
	Py_RETURN_NONE;
}

PyObject *KX_GameObject::PySetVisible(PyObject *args)
{
	int visible, recursive = 0;
	if (!PyArg_ParseTuple(args, "i|i:setVisible", &visible, &recursive)) {
		return nullptr;
	}

	SetVisible(visible ? true : false, recursive ? true : false);
	Py_RETURN_NONE;

}

PyObject *KX_GameObject::PySetOcclusion(PyObject *args)
{
	int occlusion, recursive = 0;

	if (!PyArg_ParseTuple(args, "i|i:setOcclusion", &occlusion, &recursive)) {
		return nullptr;
	}

	SetOccluder(occlusion ? true : false, recursive ? true : false);
	Py_RETURN_NONE;
}

PyObject *KX_GameObject::PyGetVelocity(PyObject *args)
{
	// only can get the velocity if we have a physics object connected to us...
	mt::vec3 point = mt::zero3;
	PyObject *pypos = nullptr;

	if (!PyArg_ParseTuple(args, "|O:getVelocity", &pypos) || (pypos && !PyVecTo(pypos, point))) {
		return nullptr;
	}

	return PyObjectFrom(GetVelocity(point));
}

PyObject *KX_GameObject::PyGetReactionForce()
{
	// only can get the velocity if we have a physics object connected to us...

	// XXX - Currently not working with bullet intergration, see KX_BulletPhysicsController.cpp's getReactionForce
#if 0
	if (GetPhysicsController1()) {
		return PyObjectFrom(GetPhysicsController1()->getReactionForce());
	}
	return PyObjectFrom(mt::zero3);
#endif

	return PyObjectFrom(mt::zero3);
}



PyObject *KX_GameObject::PyEnableRigidBody()
{
	if (m_physicsController) {
		m_physicsController->SetRigidBody(true);
	}

	Py_RETURN_NONE;
}



PyObject *KX_GameObject::PyDisableRigidBody()
{
	if (m_physicsController) {
		m_physicsController->SetRigidBody(false);
	}

	Py_RETURN_NONE;
}


PyObject *KX_GameObject::PySetParent(PyObject *args, PyObject *kwds)
{
	PyObject *pyobj;
	KX_GameObject *obj;
	int addToCompound = 1, ghost = 1;

	if (!EXP_ParseTupleArgsAndKeywords(args, kwds, "O|ii:setParent", {"parent", "compound", "ghost", 0},
	                                   &pyobj, &addToCompound, &ghost)) {
		return nullptr; // Python sets a simple error
	}
	if (!ConvertPythonToGameObject(GetScene(), pyobj, &obj, true, "gameOb.setParent(obj): KX_GameObject")) {
		return nullptr;
	}

	if (obj) {
		SetParent(obj, addToCompound, ghost);
	}
	Py_RETURN_NONE;
}

PyObject *KX_GameObject::PyRemoveParent()
{
	RemoveParent();
	Py_RETURN_NONE;
}


PyObject *KX_GameObject::PySetCollisionMargin(PyObject *value)
{
	float collisionMargin = PyFloat_AsDouble(value);

	if (collisionMargin == -1 && PyErr_Occurred()) {
		PyErr_SetString(PyExc_TypeError, "expected a float");
		return nullptr;
	}

	PYTHON_CHECK_PHYSICS_CONTROLLER(this, "setCollisionMargin", nullptr);

	m_physicsController->SetMargin(collisionMargin);
	Py_RETURN_NONE;
}


PyObject *KX_GameObject::PyCollide(PyObject *value)
{
	KX_Scene *scene = GetScene();
	KX_GameObject *other;

	if (!ConvertPythonToGameObject(scene, value, &other, false, "gameOb.collide(obj): KX_GameObject")) {
		return nullptr;
	}

	if (!m_physicsController || !other->GetPhysicsController()) {
		PyErr_SetString(PyExc_TypeError, "expected objects with physics controller");
		return nullptr;
	}

	PHY_IPhysicsEnvironment *env = scene->GetPhysicsEnvironment();
	PHY_CollisionTestResult testResult = env->CheckCollision(m_physicsController.get(), other->GetPhysicsController());

	PyObject *result = PyTuple_New(2);
	if (!testResult.collide) {
		PyTuple_SET_ITEM(result, 0, Py_False);
		PyTuple_SET_ITEM(result, 1, Py_None);
	}
	else {
		PyTuple_SET_ITEM(result, 0, Py_True);

		if (testResult.collData) {
			KX_CollisionContactPointList *contactPointList = new KX_CollisionContactPointList(testResult.collData, testResult.isFirst);
			PyTuple_SET_ITEM(result, 1, contactPointList->NewProxy(true));
		}
		else {
			PyTuple_SET_ITEM(result, 1, Py_None);
		}
	}

	return result;
}


PyObject *KX_GameObject::PyApplyImpulse(PyObject *args)
{
	PyObject *pyattach;
	PyObject *pyimpulse;
	int local = 0;

	PYTHON_CHECK_PHYSICS_CONTROLLER(this, "applyImpulse", nullptr);
	if (PyArg_ParseTuple(args, "OO|i:applyImpulse", &pyattach, &pyimpulse, &local)) {
		mt::vec3 attach;
		mt::vec3 impulse;
		if (PyVecTo(pyattach, attach) && PyVecTo(pyimpulse, impulse)) {
			m_physicsController->ApplyImpulse(attach, impulse, (local != 0));
			Py_RETURN_NONE;
		}

	}

	return nullptr;
}

PyObject *KX_GameObject::PySuspendPhysics(PyObject *args)
{
	int freeConstraints = false;

	if (!PyArg_ParseTuple(args, "|i:suspendPhysics", &freeConstraints)) {
		return nullptr;
	}

	SuspendPhysics(freeConstraints);

	Py_RETURN_NONE;
}

PyObject *KX_GameObject::PyRestorePhysics()
{
	RestorePhysics();

	Py_RETURN_NONE;
}

PyObject *KX_GameObject::PySuspendDynamics(PyObject *args)
{
	bool ghost = false;

	if (!PyArg_ParseTuple(args, "|b", &ghost)) {
		return nullptr;
	}

	if (m_physicsController) {
		m_physicsController->SuspendDynamics(ghost);
	}

	Py_RETURN_NONE;
}



PyObject *KX_GameObject::PyRestoreDynamics()
{
	// Child objects must be static, so we block changing to dynamic
	if (m_physicsController && !GetParent()) {
		m_physicsController->RestoreDynamics();
	}
	Py_RETURN_NONE;
}


PyObject *KX_GameObject::PyAlignAxisToVect(PyObject *args, PyObject *kwds)
{
	PyObject *pyvect;
	int axis = 2; //z axis is the default
	float fac = 1.0f;

	if (EXP_ParseTupleArgsAndKeywords(args, kwds, "O|if:alignAxisToVect", {"vect", "axis", "factor", 0},
	                                  &pyvect, &axis, &fac)) {
		mt::vec3 vect;
		if (PyVecTo(pyvect, vect)) {
			if (fac > 0.0f) {
				if (fac > 1.0f) {
					fac = 1.0f;
				}

				AlignAxisToVect(vect, axis, fac);
				NodeUpdate();
			}
			Py_RETURN_NONE;
		}
	}
	return nullptr;
}

PyObject *KX_GameObject::PyGetAxisVect(PyObject *value)
{
	mt::vec3 vect;
	if (PyVecTo(value, vect)) {
		return PyObjectFrom(NodeGetWorldOrientation() * vect);
	}
	return nullptr;
}


PyObject *KX_GameObject::PyGetPhysicsId()
{
	unsigned long long physid = 0; // TODO work with PyCapsule
	if (m_physicsController) {
		physid = (unsigned long long)m_physicsController.get();
	}
	return PyLong_FromUnsignedLongLong(physid);
}

PyObject *KX_GameObject::PyGetPropertyNames()
{
	return ConvertKeysToPython();
}

EXP_PYMETHODDEF_DOC_O(KX_GameObject, getDistanceTo,
                      "getDistanceTo(other): get distance to another point/KX_GameObject")
{
	mt::vec3 b;
	if (PyVecTo(value, b)) {
		return PyFloat_FromDouble((NodeGetWorldPosition() - b).Length());
	}
	PyErr_Clear();

	KX_GameObject *other;
	if (ConvertPythonToGameObject(GetScene(), value, &other, false, "gameOb.getDistanceTo(value): KX_GameObject")) {
		return PyFloat_FromDouble((NodeGetWorldPosition() - other->NodeGetWorldPosition()).Length());
	}

	return nullptr;
}

EXP_PYMETHODDEF_DOC_O(KX_GameObject, getVectTo,
                      "getVectTo(other): get vector and the distance to another point/KX_GameObject\n"
                      "Returns a 3-tuple with (distance,worldVector,localVector)\n")
{
	mt::vec3 toPoint, fromPoint;
	mt::vec3 toDir, locToDir;
	float distance;

	PyObject *returnValue;

	if (!PyVecTo(value, toPoint)) {
		PyErr_Clear();

		KX_GameObject *other;
		if (ConvertPythonToGameObject(GetScene(), value, &other, false, "")) { /* error will be overwritten */
			toPoint = other->NodeGetWorldPosition();
		}
		else {
			PyErr_SetString(PyExc_TypeError, "gameOb.getVectTo(other): KX_GameObject, expected a 3D Vector or KX_GameObject type");
			return nullptr;
		}
	}

	fromPoint = NodeGetWorldPosition();
	toDir = toPoint - fromPoint;
	distance = toDir.Length();

	if (mt::FuzzyZero(distance)) {
		locToDir = toDir = mt::zero3;
		distance = 0.0f;
	}
	else {
		toDir.Normalize();
		locToDir = toDir * NodeGetWorldOrientation();
	}

	returnValue = PyTuple_New(3);
	if (returnValue) { // very unlikely to fail, python sets a memory error here.
		PyTuple_SET_ITEM(returnValue, 0, PyFloat_FromDouble(distance));
		PyTuple_SET_ITEM(returnValue, 1, PyObjectFrom(toDir));
		PyTuple_SET_ITEM(returnValue, 2, PyObjectFrom(locToDir));
	}
	return returnValue;
}

struct KX_GameObject::RayCastData {
	RayCastData(std::string prop, bool xray, unsigned int mask)
		:m_prop(prop),
		m_xray(xray),
		m_mask(mask),
		m_hitObject(nullptr)
	{
	}

	std::string m_prop;
	bool m_xray;
	unsigned int m_mask;
	KX_GameObject *m_hitObject;
};

static bool CheckRayCastObject(KX_GameObject *obj, KX_GameObject::RayCastData *rayData)
{
	const std::string& prop = rayData->m_prop;
	const unsigned short mask = rayData->m_mask;
	// Check if the object had a given property (if this one is non empty) and have the correct group mask (if this one is different from 0xFFFF).
	return ((prop.empty() || obj->GetProperty(prop)) && (mask == ((1u << OB_MAX_COL_MASKS) - 1) || obj->GetCollisionGroup() & mask));
}

bool KX_GameObject::RayHit(KX_ClientObjectInfo *client, KX_RayCast *result, RayCastData *rayData)
{
	KX_GameObject *obj = client->m_gameobject;

	// if X-ray option is selected, the unwanted objects were not tested, so get here only with true hit
	// if not, all objects were tested and the front one may not be the correct one.
	if (rayData->m_xray || CheckRayCastObject(obj, rayData)) {
		rayData->m_hitObject = obj;
	}
	// return true to stop RayCast::RayTest from looping, the above test was decisive
	// We would want to loop only if we want to get more than one hit point
	return true;
}

/* this function is used to pre-filter the object before casting the ray on them.
 * This is useful for "X-Ray" option when we want to see "through" unwanted object.
 */
bool KX_GameObject::NeedRayCast(KX_ClientObjectInfo *client, RayCastData *rayData)
{
	KX_GameObject *obj = client->m_gameobject;

	// if X-Ray option is selected, skip object that don't match the criteria as we see through them
	// if not, test all objects because we don't know yet which one will be on front
	return (!rayData->m_xray || CheckRayCastObject(obj, rayData));
}

EXP_PYMETHODDEF_DOC(KX_GameObject, rayCastTo,
                    "rayCastTo(other,dist,prop): look towards another point/KX_GameObject and return first object hit within dist that matches prop\n"
                    " prop = property name that object must have; can be omitted => detect any object\n"
                    " dist = max distance to look (can be negative => look behind); 0 or omitted => detect up to other\n"
                    " other = 3-tuple or object reference")
{
	mt::vec3 toPoint;
	PyObject *pyarg;
	float dist = 0.0f;
	const char *propName = "";

	if (!EXP_ParseTupleArgsAndKeywords(args, kwds, "O|fs:rayCastTo", {"other", "dist", "prop", 0},
	                                   &pyarg, &dist, &propName)) {
		return nullptr; // python sets simple error
	}

	if (!PyVecTo(pyarg, toPoint)) {
		KX_GameObject *other;
		PyErr_Clear();

		if (ConvertPythonToGameObject(GetScene(), pyarg, &other, false, "")) { /* error will be overwritten */
			toPoint = other->NodeGetWorldPosition();
		}
		else {
			PyErr_SetString(PyExc_TypeError, "gameOb.rayCastTo(other,dist,prop): KX_GameObject, the first argument to rayCastTo must be a vector or a KX_GameObject");
			return nullptr;
		}
	}
	mt::vec3 fromPoint = NodeGetWorldPosition();

	if (dist != 0.0f) {
		toPoint = fromPoint + dist * (toPoint - fromPoint).SafeNormalized(mt::axisX3);
	}

	PHY_IPhysicsEnvironment *pe = GetScene()->GetPhysicsEnvironment();
	PHY_IPhysicsController *spc = m_physicsController.get();
	KX_GameObject *parent = GetParent();
	if (!spc && parent) {
		spc = parent->GetPhysicsController();
	}

	RayCastData rayData(propName, false, (1u << OB_MAX_COL_MASKS) - 1);
	KX_RayCast::Callback<KX_GameObject, RayCastData> callback(this, spc, &rayData);
	if (KX_RayCast::RayTest(pe, fromPoint, toPoint, callback) && rayData.m_hitObject) {
		return rayData.m_hitObject->GetProxy();
	}

	Py_RETURN_NONE;
}

/* faster then Py_BuildValue since some scripts call raycast a lot */
static PyObject *none_tuple_3()
{
	PyObject *ret = PyTuple_New(3);
	PyTuple_SET_ITEM(ret, 0, Py_None);
	PyTuple_SET_ITEM(ret, 1, Py_None);
	PyTuple_SET_ITEM(ret, 2, Py_None);

	Py_INCREF(Py_None);
	Py_INCREF(Py_None);
	Py_INCREF(Py_None);
	return ret;
}
static PyObject *none_tuple_4()
{
	PyObject *ret = PyTuple_New(4);
	PyTuple_SET_ITEM(ret, 0, Py_None);
	PyTuple_SET_ITEM(ret, 1, Py_None);
	PyTuple_SET_ITEM(ret, 2, Py_None);
	PyTuple_SET_ITEM(ret, 3, Py_None);

	Py_INCREF(Py_None);
	Py_INCREF(Py_None);
	Py_INCREF(Py_None);
	Py_INCREF(Py_None);
	return ret;
}

static PyObject *none_tuple_5()
{
	PyObject *ret = PyTuple_New(5);
	PyTuple_SET_ITEM(ret, 0, Py_None);
	PyTuple_SET_ITEM(ret, 1, Py_None);
	PyTuple_SET_ITEM(ret, 2, Py_None);
	PyTuple_SET_ITEM(ret, 3, Py_None);
	PyTuple_SET_ITEM(ret, 4, Py_None);

	Py_INCREF(Py_None);
	Py_INCREF(Py_None);
	Py_INCREF(Py_None);
	Py_INCREF(Py_None);
	Py_INCREF(Py_None);
	return ret;
}

EXP_PYMETHODDEF_DOC(KX_GameObject, rayCast,
                    "rayCast(to,from,dist,prop,face,xray,poly,mask): cast a ray and return 3-tuple (object,hit,normal) or 4-tuple (object,hit,normal,polygon) or 4-tuple (object,hit,normal,polygon,hituv) of contact point with object within dist that matches prop.\n"
                    " If no hit, return (None,None,None) or (None,None,None,None) or (None,None,None,None,None).\n"
                    " to   = 3-tuple or object reference for destination of ray (if object, use center of object)\n"
                    " from = 3-tuple or object reference for origin of ray (if object, use center of object)\n"
                    "        Can be None or omitted => start from self object center\n"
                    " dist = max distance to look (can be negative => look behind); 0 or omitted => detect up to to\n"
                    " prop = property name that object must have; can be omitted => detect any object\n"
                    " face = normal option: 1=>return face normal; 0 or omitted => normal is oriented towards origin\n"
                    " xray = X-ray option: 1=>skip objects that don't match prop; 0 or omitted => stop on first object\n"
                    " poly = polygon option: 1=>return value is a 4-tuple and the 4th element is a KX_PolyProxy object\n"
                    "                           which can be None if hit object has no mesh or if there is no hit\n"
                    "                        2=>return value is a 5-tuple, the 4th element is the KX_PolyProxy object\n"
                    "                           and the 5th element is the vector of UV coordinates at the hit point of the None if there is no UV mapping\n"
                    "        If 0 or omitted, return value is a 3-tuple\n"
                    " mask = collision mask: the collision mask that ray can hit, 0 < mask < 65536\n"
                    "Note: The object on which you call this method matters: the ray will ignore it.\n"
                    "      prop and xray option interact as follow:\n"
                    "        prop off, xray off: return closest hit or no hit if there is no object on the full extend of the ray\n"
                    "        prop off, xray on : idem\n"
                    "        prop on,  xray off: return closest hit if it matches prop, no hit otherwise\n"
                    "        prop on,  xray on : return closest hit matching prop or no hit if there is no object matching prop on the full extend of the ray\n")
{
	mt::vec3 toPoint;
	mt::vec3 fromPoint;
	PyObject *pyto;
	PyObject *pyfrom = Py_None;
	float dist = 0.0f;
	const char *propName = "";
	KX_GameObject *other;
	int face = 0, xray = 0, poly = 0;
	int mask = (1 << OB_MAX_COL_MASKS) - 1;

	if (!EXP_ParseTupleArgsAndKeywords(args, kwds, "O|Ofsiiii:rayCast",
	                                   {"objto", "objfrom", "dist", "prop", "face", "xray", "poly", "mask", 0},
	                                   &pyto, &pyfrom, &dist, &propName, &face, &xray, &poly, &mask)) {
		return nullptr; // Python sets a simple error
	}

	KX_Scene *scene = GetScene();

	if (!PyVecTo(pyto, toPoint)) {
		PyErr_Clear();

		if (ConvertPythonToGameObject(scene, pyto, &other, false, "")) { /* error will be overwritten */
			toPoint = other->NodeGetWorldPosition();
		}
		else {
			PyErr_SetString(PyExc_TypeError, "the first argument to rayCast must be a vector or a KX_GameObject");
			return nullptr;
		}
	}
	if (pyfrom == Py_None) {
		fromPoint = NodeGetWorldPosition();
	}
	else if (!PyVecTo(pyfrom, fromPoint)) {
		PyErr_Clear();

		if (ConvertPythonToGameObject(scene, pyfrom, &other, false, "")) { /* error will be overwritten */
			fromPoint = other->NodeGetWorldPosition();
		}
		else {
			PyErr_SetString(PyExc_TypeError, "gameOb.rayCast(to,from,dist,prop,face,xray,poly,mask): KX_GameObject, the second optional argument to rayCast must be a vector or a KX_GameObject");
			return nullptr;
		}
	}

	if (mask == 0 || mask & ~((1 << OB_MAX_COL_MASKS) - 1)) {
		PyErr_Format(PyExc_TypeError, "gameOb.rayCast(to,from,dist,prop,face,xray,poly,mask): KX_GameObject, mask argument to rayCast must be a int bitfield, 0 < mask < %i", (1 << OB_MAX_COL_MASKS));
		return nullptr;
	}

	if (dist != 0.0f) {
		mt::vec3 toDir = toPoint - fromPoint;
		if (mt::FuzzyZero(toDir.LengthSquared())) {
			//return Py_BuildValue("OOO", Py_None, Py_None, Py_None);
			return none_tuple_3();
		}
		toDir.Normalize();
		toPoint = fromPoint + (dist) * toDir;
	}
	else if (mt::FuzzyZero((toPoint - fromPoint).LengthSquared())) {
		//return Py_BuildValue("OOO", Py_None, Py_None, Py_None);
		return none_tuple_3();
	}

	PHY_IPhysicsEnvironment *pe = scene->GetPhysicsEnvironment();
	PHY_IPhysicsController *spc = m_physicsController.get();
	KX_GameObject *parent = GetParent();
	if (!spc && parent) {
		spc = parent->GetPhysicsController();
	}

	// to get the hit results
	RayCastData rayData(propName, xray, mask);
	KX_RayCast::Callback<KX_GameObject, RayCastData> callback(this, spc, &rayData, face, (poly == 2));

	if (KX_RayCast::RayTest(pe, fromPoint, toPoint, callback) && rayData.m_hitObject) {
		PyObject *returnValue = (poly == 2) ? PyTuple_New(5) : (poly) ? PyTuple_New(4) : PyTuple_New(3);
		if (returnValue) { // unlikely this would ever fail, if it does python sets an error
			PyTuple_SET_ITEM(returnValue, 0, rayData.m_hitObject->GetProxy());
			PyTuple_SET_ITEM(returnValue, 1, PyObjectFrom(callback.m_hitPoint));
			PyTuple_SET_ITEM(returnValue, 2, PyObjectFrom(callback.m_hitNormal));
			if (poly) {
				if (callback.m_hitMesh) {
					KX_Mesh *mesh = static_cast<KX_Mesh *>(callback.m_hitMesh);
					// if this field is set, then we can trust that m_hitPolygon is a valid polygon
					const RAS_Mesh::PolygonInfo polygon = mesh->GetPolygon(callback.m_hitPolygon);
					KX_PolyProxy *polyproxy = new KX_PolyProxy(mesh, polygon);
					PyTuple_SET_ITEM(returnValue, 3, polyproxy->NewProxy(true));
					if (poly == 2) {
						if (callback.m_hitUVOK) {
							PyTuple_SET_ITEM(returnValue, 4, PyObjectFrom(callback.m_hitUV));
						}
						else {
							Py_INCREF(Py_None);
							PyTuple_SET_ITEM(returnValue, 4, Py_None);
						}
					}
				}
				else {
					Py_INCREF(Py_None);
					PyTuple_SET_ITEM(returnValue, 3, Py_None);
					if (poly == 2) {
						Py_INCREF(Py_None);
						PyTuple_SET_ITEM(returnValue, 4, Py_None);
					}
				}
			}
		}
		return returnValue;
	}
	// no hit
	if (poly == 2) {
		return none_tuple_5();
	}
	else if (poly) {
		return none_tuple_4();
	}
	else {
		return none_tuple_3();
	}
}

EXP_PYMETHODDEF_DOC(KX_GameObject, sendMessage,
                    "sendMessage(subject, [body, to])\n"
                    "sends a message in same manner as a message actuator"
                    "subject = Subject of the message (string)"
                    "body = Message body (string)"
                    "to = Name of object to send the message to")
{
	char *subject;
	char *body = (char *)"";
	char *to = (char *)"";

	if (!EXP_ParseTupleArgsAndKeywords(args, kwds, "s|ss:sendMessage", {"subject", "body", "to", 0},
	                                   &subject, &body, &to)) {
		return nullptr;
	}

	GetScene()->GetNetworkMessageScene()->SendMessage(to, this, subject, body);
	Py_RETURN_NONE;
}

static void layer_check(short &layer, const char *method_name)
{
	if (layer < 0 || layer >= MAX_ACTION_LAYERS) {
		CM_PythonFunctionWarning("KX_GameObject", method_name, "given layer (" << layer
		                                                                       << ") is out of range (0 - " << (MAX_ACTION_LAYERS - 1) << "), setting to 0.");
		layer = 0;
	}
}

EXP_PYMETHODDEF_DOC(KX_GameObject, playAction,
                    "playAction(name, start_frame, end_frame, layer=0, priority=0 blendin=0, play_mode=ACT_MODE_PLAY, layer_weight=0.0, ipo_flags=0, speed=1.0)\n"
                    "Plays an action\n")
{
	const char *name;
	float start, end, blendin = 0.f, speed = 1.f, layer_weight = 0.f;
	short layer = 0, priority = 0;
	short ipo_flags = 0;
	short play_mode = 0;
	short blend_mode = 0;

	if (!EXP_ParseTupleArgsAndKeywords(args, kwds, "sff|hhfhfhfh:playAction", {"name", "start_frame", "end_frame", "layer",
	                                                                           "priority", "blendin", "play_mode", "layer_weight", "ipo_flags", "speed", "blend_mode", 0},
	                                   &name, &start, &end, &layer, &priority, &blendin, &play_mode, &layer_weight, &ipo_flags, &speed, &blend_mode)) {
		return nullptr;
	}

	layer_check(layer, "playAction");

	if (play_mode < 0 || play_mode > BL_Action::ACT_MODE_MAX) {
		CM_PythonFunctionWarning("KX_GameObject", "playAction", "given play_mode (" << play_mode << ") is out of range (0 - "
		                                                                            << (BL_Action::ACT_MODE_MAX - 1) << "), setting to ACT_MODE_PLAY");
		play_mode = BL_Action::ACT_MODE_PLAY;
	}

	if (blend_mode < 0 || blend_mode > BL_Action::ACT_BLEND_MAX) {
		CM_PythonFunctionWarning("KX_GameObject", "playAction", "given blend_mode (" << blend_mode << ") is out of range (0 - "
		                                                                             << (BL_Action::ACT_BLEND_MAX - 1) << "), setting to ACT_BLEND_BLEND");
		blend_mode = BL_Action::ACT_BLEND_BLEND;
	}

	if (layer_weight < 0.f || layer_weight > 1.f) {
		CM_PythonFunctionWarning("KX_GameObject", "playAction", "given layer_weight (" << layer_weight
		                                                                               << ") is out of range (0.0 - 1.0), setting to 0.0");
		layer_weight = 0.f;
	}

	PlayAction(name, start, end, layer, priority, blendin, play_mode, layer_weight, ipo_flags, speed, blend_mode);

	Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(KX_GameObject, stopAction,
                    "stopAction(layer=0)\n"
                    "Stop playing the action on the given layer\n")
{
	short layer = 0;

	if (!PyArg_ParseTuple(args, "|h:stopAction", &layer)) {
		return nullptr;
	}

	layer_check(layer, "stopAction");

	StopAction(layer);

	Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(KX_GameObject, getActionFrame,
                    "getActionFrame(layer=0)\n"
                    "Gets the current frame of the action playing in the supplied layer\n")
{
	short layer = 0;

	if (!PyArg_ParseTuple(args, "|h:getActionFrame", &layer)) {
		return nullptr;
	}

	layer_check(layer, "getActionFrame");

	return PyFloat_FromDouble(GetActionFrame(layer));
}

EXP_PYMETHODDEF_DOC(KX_GameObject, getActionName,
                    "getActionName(layer=0)\n"
                    "Gets the name of the current action playing in the supplied layer\n")
{
	short layer = 0;

	if (!PyArg_ParseTuple(args, "|h:getActionName", &layer)) {
		return nullptr;
	}

	layer_check(layer, "getActionName");

	return PyUnicode_FromStdString(GetActionName(layer));
}

EXP_PYMETHODDEF_DOC(KX_GameObject, setActionFrame,
                    "setActionFrame(frame, layer=0)\n"
                    "Set the current frame of the action playing in the supplied layer\n")
{
	short layer = 0;
	float frame;

	if (!PyArg_ParseTuple(args, "f|h:setActionFrame", &frame, &layer)) {
		return nullptr;
	}

	layer_check(layer, "setActionFrame");

	SetActionFrame(layer, frame);

	Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(KX_GameObject, isPlayingAction,
                    "isPlayingAction(layer=0)\n"
                    "Checks to see if there is an action playing in the given layer\n")
{
	short layer = 0;

	if (!PyArg_ParseTuple(args, "|h:isPlayingAction", &layer)) {
		return nullptr;
	}

	layer_check(layer, "isPlayingAction");

	return PyBool_FromLong(!IsActionDone(layer));
}


EXP_PYMETHODDEF_DOC(KX_GameObject, addDebugProperty,
                    "addDebugProperty(name, visible=1)\n"
                    "Added or remove a debug property to the debug list.\n")
{
	KX_Scene *scene = GetScene();
	char *name;
	int visible = 1;

	if (!PyArg_ParseTuple(args, "s|i:debugProperty", &name, &visible)) {
		return nullptr;
	}

	if (visible) {
		if (!scene->PropertyInDebugList(this, name)) {
			scene->AddDebugProperty(this, name);
		}
	}
	else {
		scene->RemoveDebugProperty(this, name);
	}

	Py_RETURN_NONE;
}


/* dict style access */


/* Matches python dict.get(key, [default]) */
PyObject *KX_GameObject::Pyget(PyObject *args)
{
	PyObject *key;
	PyObject *def = Py_None;

	if (!PyArg_ParseTuple(args, "O|O:get", &key, &def)) {
		return nullptr;
	}


	if (PyUnicode_Check(key)) {
		EXP_PropValue *item = GetProperty(_PyUnicode_AsString(key));
		if (item) {
			return item->ConvertValueToPython();
		}
	}

	Py_INCREF(def);
	return def;
}

bool ConvertPythonToGameObject(KX_Scene *scene, PyObject *value, KX_GameObject **object, bool py_none_ok, const char *error_prefix)
{
	if (value == nullptr) {
		PyErr_Format(PyExc_TypeError, "%s, python pointer nullptr, should never happen", error_prefix);
		*object = nullptr;
		return false;
	}

	if (value == Py_None) {
		*object = nullptr;

		if (py_none_ok) {
			return true;
		}
		else {
			PyErr_Format(PyExc_TypeError, "%s, expected KX_GameObject or a KX_GameObject name, None is invalid", error_prefix);
			return false;
		}
	}

	if (PyUnicode_Check(value)) {
		*object = scene->GetResources().FindObject(std::string(_PyUnicode_AsString(value)));

		if (*object) {
			return true;
		}
		else {
			PyErr_Format(PyExc_ValueError, "%s, requested name \"%s\" did not match any KX_GameObject in this scene", error_prefix, _PyUnicode_AsString(value));
			return false;
		}
	}

	if (PyObject_TypeCheck(value, &KX_GameObject::Type) ||
	    PyObject_TypeCheck(value, &KX_LightObject::Type)    ||
	    PyObject_TypeCheck(value, &KX_Camera::Type)         ||
	    PyObject_TypeCheck(value, &KX_FontObject::Type) ||
	    PyObject_TypeCheck(value, &KX_NavMeshObject::Type)) {
		*object = static_cast<KX_GameObject *>EXP_PROXY_REF(value);

		/* sets the error */
		if (*object == nullptr) {
			PyErr_Format(PyExc_SystemError, "%s, " EXP_PROXY_ERROR_MSG, error_prefix);
			return false;
		}

		return true;
	}

	*object = nullptr;

	if (py_none_ok) {
		PyErr_Format(PyExc_TypeError, "%s, expect a KX_GameObject, a string or None", error_prefix);
	}
	else {
		PyErr_Format(PyExc_TypeError, "%s, expect a KX_GameObject or a string", error_prefix);
	}

	return false;
}
#endif // WITH_PYTHON
