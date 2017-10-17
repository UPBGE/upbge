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
 */

/** \file gameengine/Converter/BL_DeformableGameObject.cpp
 *  \ingroup bgeconv
 */

#include "BL_DeformableGameObject.h"
#include "BL_ShapeDeformer.h"
#include "BL_ModifierDeformer.h"
#include "BL_SkinDeformer.h"
#include "BL_MeshDeformer.h"

#include "KX_SoftBodyDeformer.h"

#include "RAS_MeshObject.h"

#include "CM_Message.h"

BL_DeformableGameObject::BL_DeformableGameObject(void *sgReplicationInfo, SG_Callbacks callbacks)
	:KX_GameObject(sgReplicationInfo, callbacks),
	m_pDeformer(nullptr),
	m_lastframe(0.0),
	m_activePriority(9999)
{
}

BL_DeformableGameObject::~BL_DeformableGameObject()
{
	if (m_pDeformer) {
		delete m_pDeformer;
	}
}

EXP_Value *BL_DeformableGameObject::GetReplica()
{
	BL_DeformableGameObject *replica = new BL_DeformableGameObject(*this);
	replica->ProcessReplica();
	return replica;
}

void BL_DeformableGameObject::ProcessReplica()
{
	KX_GameObject::ProcessReplica();

	if (m_pDeformer) {
		m_pDeformer = m_pDeformer->GetReplica();
	}
}

void BL_DeformableGameObject::Relink(std::map<SCA_IObject *, SCA_IObject *>& map)
{
	if (m_pDeformer) {
		m_pDeformer->Relink(map);
	}
	KX_GameObject::Relink(map);
}

double BL_DeformableGameObject::GetLastFrame() const
{
	return m_lastframe;
}

bool BL_DeformableGameObject::SetActiveAction(short priority, double curtime)
{
	if (curtime != m_lastframe) {
		m_activePriority = 9999;
		m_lastframe = curtime;
	}

	if (priority <= m_activePriority) {
		m_activePriority = priority;
		m_lastframe = curtime;

		return true;
	}
	else {
		return false;
	}
}

bool BL_DeformableGameObject::GetShape(std::vector<float> &shape)
{
	shape.clear();
	BL_ShapeDeformer *shape_deformer = dynamic_cast<BL_ShapeDeformer *>(m_pDeformer);
	if (shape_deformer) {
		// this check is normally superfluous: a shape deformer can only be created if the mesh
		// has relative keys
		Key *key = shape_deformer->GetKey();
		if (key && key->type == KEY_RELATIVE) {
			KeyBlock *kb;
			for (kb = (KeyBlock *)key->block.first; kb; kb = (KeyBlock *)kb->next) {
				shape.push_back(kb->curval);
			}
		}
	}
	return !shape.empty();
}

void BL_DeformableGameObject::SetDeformer(RAS_Deformer *deformer)
{
	// Make sure that the object doesn't already have a mesh user.
	BLI_assert(m_meshUser == nullptr);
	m_pDeformer = deformer;
}

RAS_Deformer *BL_DeformableGameObject::GetDeformer()
{
	return m_pDeformer;
}

bool BL_DeformableGameObject::IsDeformable() const
{
	return true;
}

void BL_DeformableGameObject::LoadDeformer()
{
	if (m_pDeformer) {
		delete m_pDeformer;
		m_pDeformer = nullptr;
	}

	if (m_meshes.empty()) {
		return;
	}

	RAS_MeshObject *meshobj = m_meshes.front();
	Mesh *mesh = meshobj->GetMesh();

	if (!mesh) {
		return;
	}

	KX_Scene *scene = GetScene();
	Scene *blenderScene = scene->GetBlenderScene();
	// We must create a new deformer but which one?
	KX_GameObject *parentobj = GetParent();
	// Object that owns the mesh.
	Object *oldblendobj = static_cast<Object *>(scene->GetLogicManager()->FindBlendObjByGameMeshName(meshobj->GetName()));

	bool bHasModifier = BL_ModifierDeformer::HasCompatibleDeformer(m_pBlenderObject);
	bool bHasShapeKey = mesh->key && mesh->key->type==KEY_RELATIVE;
	bool bHasDvert = mesh->dvert;
	bool bHasArmature = BL_ModifierDeformer::HasArmatureDeformer(m_pBlenderObject) &&
		parentobj && parentobj->GetGameObjectType() == SCA_IObject::OBJ_ARMATURE && oldblendobj && bHasDvert;
#ifdef WITH_BULLET
	bool bHasSoftBody = (!parentobj && (m_pBlenderObject->gameflag & OB_SOFT_BODY));
#endif

	if (!oldblendobj) {
		if (bHasModifier || bHasShapeKey || bHasDvert || bHasArmature) {
			CM_FunctionWarning("new mesh is not used in an object from the current scene, you will get incorrect behavior.");
			return;
		}
	}

	if (bHasModifier) {
		if (bHasShapeKey || bHasArmature) {
			BL_ModifierDeformer *modifierDeformer = new BL_ModifierDeformer(this, blenderScene, oldblendobj, m_pBlenderObject,
					meshobj, static_cast<BL_ArmatureObject *>(parentobj));
			modifierDeformer->LoadShapeDrivers(parentobj);
			m_pDeformer = modifierDeformer;
		}
		else {
			m_pDeformer = new BL_ModifierDeformer(this, blenderScene, oldblendobj, m_pBlenderObject, meshobj, nullptr);
		}
	}
	else if (bHasShapeKey) {
		if (bHasArmature) {
			BL_ShapeDeformer *shapeDeformer = new BL_ShapeDeformer(this, oldblendobj, m_pBlenderObject, meshobj,
					static_cast<BL_ArmatureObject *>(parentobj));
			shapeDeformer->LoadShapeDrivers(parentobj);
			m_pDeformer = shapeDeformer;
		}
		else {
			m_pDeformer = new BL_ShapeDeformer(this, oldblendobj, m_pBlenderObject, meshobj, nullptr);
		}
	}
	else if (bHasArmature) {
		m_pDeformer = new BL_SkinDeformer(this, oldblendobj, m_pBlenderObject, meshobj,
				static_cast<BL_ArmatureObject *>(parentobj));
	}
	else if (bHasDvert) {
		m_pDeformer = new BL_MeshDeformer(this, oldblendobj, meshobj);
	}
#ifdef WITH_BULLET
	else if (bHasSoftBody) {
		m_pDeformer = new KX_SoftBodyDeformer(meshobj, this);
	}
#endif
}

