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

#include "RAS_Mesh.h"

#include "CM_Message.h"

BL_DeformableGameObject::BL_DeformableGameObject(void *sgReplicationInfo, SG_Callbacks callbacks)
	:KX_GameObject(sgReplicationInfo, callbacks),
	m_deformer(nullptr),
	m_lastframe(0.0)
{
}

BL_DeformableGameObject::~BL_DeformableGameObject()
{
	if (m_deformer) {
		delete m_deformer;
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

	if (m_deformer) {
		m_deformer = m_deformer->GetReplica();
	}
}

void BL_DeformableGameObject::Relink(std::map<SCA_IObject *, SCA_IObject *>& map)
{
	if (m_deformer) {
		m_deformer->Relink(map);
	}
	KX_GameObject::Relink(map);
}

double BL_DeformableGameObject::GetLastFrame() const
{
	return m_lastframe;
}

void BL_DeformableGameObject::SetLastFrame(double curtime)
{
	m_lastframe = curtime;
}

bool BL_DeformableGameObject::GetShape(std::vector<float> &shape)
{
	shape.clear();
	BL_ShapeDeformer *shape_deformer = dynamic_cast<BL_ShapeDeformer *>(m_deformer);
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
	m_deformer = deformer;
}

RAS_Deformer *BL_DeformableGameObject::GetDeformer()
{
	return m_deformer;
}

bool BL_DeformableGameObject::IsDeformable() const
{
	return true;
}

void BL_DeformableGameObject::LoadDeformer()
{
	if (m_deformer) {
		delete m_deformer;
		m_deformer = nullptr;
	}

	if (m_meshes.empty()) {
		return;
	}

	RAS_Mesh *meshobj = m_meshes.front();
	Mesh *mesh = meshobj->GetMesh();

	if (!mesh) {
		return;
	}

	KX_Scene *scene = GetScene();
	Scene *blenderScene = scene->GetBlenderScene();
	// We must create a new deformer but which one?
	KX_GameObject *parentobj = GetParent();
	/* Object that owns the mesh. If this is not the current blender object, look at one of the object registered
	 * along the blender mesh. */
	Object *meshblendobj;
	Object *blenderobj = GetBlenderObject();
	if (blenderobj->data != mesh) {
		meshblendobj = static_cast<Object *>(scene->GetLogicManager()->FindBlendObjByGameMeshName(meshobj->GetName()));
	}
	else {
		meshblendobj = blenderobj;
	}

	const bool isParentArmature = parentobj && parentobj->GetGameObjectType() == SCA_IObject::OBJ_ARMATURE;
	const bool bHasModifier = BL_ModifierDeformer::HasCompatibleDeformer(blenderobj);
	const bool bHasShapeKey = mesh->key && mesh->key->type == KEY_RELATIVE;
	const bool bHasDvert = mesh->dvert && blenderobj->defbase.first;
	const bool bHasArmature = BL_ModifierDeformer::HasArmatureDeformer(blenderobj) &&
			isParentArmature && meshblendobj && bHasDvert;
#ifdef WITH_BULLET
	const bool bHasSoftBody = (!parentobj && (blenderobj->gameflag & OB_SOFT_BODY));
#endif

	if (!meshblendobj) {
		if (bHasModifier || bHasShapeKey || bHasDvert || bHasArmature) {
			CM_FunctionWarning("new mesh is not used in an object from the current scene, you will get incorrect behavior.");
			return;
		}
	}

	if (bHasModifier) {
		if (isParentArmature) {
			BL_ModifierDeformer *modifierDeformer = new BL_ModifierDeformer(this, blenderScene, meshblendobj, blenderobj,
					meshobj, static_cast<BL_ArmatureObject *>(parentobj));
			modifierDeformer->LoadShapeDrivers(parentobj);
			m_deformer = modifierDeformer;
		}
		else {
			m_deformer = new BL_ModifierDeformer(this, blenderScene, meshblendobj, blenderobj, meshobj, nullptr);
		}
	}
	else if (bHasShapeKey) {
		if (isParentArmature) {
			BL_ShapeDeformer *shapeDeformer = new BL_ShapeDeformer(this, meshblendobj, blenderobj, meshobj,
					static_cast<BL_ArmatureObject *>(parentobj));
			shapeDeformer->LoadShapeDrivers(parentobj);
			m_deformer = shapeDeformer;
		}
		else {
			m_deformer = new BL_ShapeDeformer(this, meshblendobj, blenderobj, meshobj, nullptr);
		}
	}
	else if (bHasArmature) {
		m_deformer = new BL_SkinDeformer(this, meshblendobj, blenderobj, meshobj,
				static_cast<BL_ArmatureObject *>(parentobj));
	}
	else if (bHasDvert) {
		m_deformer = new BL_MeshDeformer(this, meshblendobj, meshobj);
	}
#ifdef WITH_BULLET
	else if (bHasSoftBody) {
		m_deformer = new KX_SoftBodyDeformer(meshobj, this);
	}
#endif

	if (m_deformer) {
		m_deformer->InitializeDisplayArrays();
	}
}
