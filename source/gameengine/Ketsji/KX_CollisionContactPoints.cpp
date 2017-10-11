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
 * Contributor(s): Tristan Porteries.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Ketsji/KX_CollisionContactPoints.cpp
 *  \ingroup ketsji
 */

#include "KX_CollisionContactPoints.h"
#include "PHY_DynamicTypes.h"
#include "KX_PyMath.h"

KX_CollisionContactPoint::KX_CollisionContactPoint(const PHY_ICollData *collData, unsigned int index, bool firstObject)
	:m_collData(collData),
	m_index(index),
	m_firstObject(firstObject)
{
}

KX_CollisionContactPoint::~KX_CollisionContactPoint()
{
}

std::string KX_CollisionContactPoint::GetName() const
{
	return "CollisionContactPoint";
}

#ifdef WITH_PYTHON

PyTypeObject KX_CollisionContactPoint::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"KX_CollisionContactPoint",
	sizeof(EXP_PyObjectPlus_Proxy),
	0,
	py_base_dealloc,
	0,
	0,
	0,
	0,
	py_base_repr,
	0, 0, 0, 0, 0, 0, 0, 0, 0,
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	0, 0, 0, 0, 0, 0, 0,
	Methods,
	0,
	0,
	&EXP_Value::Type,
	0, 0, 0, 0, 0, 0,
	py_base_new
};

PyMethodDef KX_CollisionContactPoint::Methods[] = {
	{nullptr, nullptr} //Sentinel
};

EXP_Attribute KX_CollisionContactPoint::Attributes[] = {
	EXP_ATTRIBUTE_RO_FUNCTION("localPointA", pyattr_get_local_point_a),
	EXP_ATTRIBUTE_RO_FUNCTION("localPointB", pyattr_get_local_point_b),
	EXP_ATTRIBUTE_RO_FUNCTION("worldPoint", pyattr_get_world_point),
	EXP_ATTRIBUTE_RO_FUNCTION("normal", pyattr_get_normal),
	EXP_ATTRIBUTE_RO_FUNCTION("combinedFriction", pyattr_get_combined_friction),
	EXP_ATTRIBUTE_RO_FUNCTION("combinedRollingFriction", pyattr_get_combined_rolling_friction),
	EXP_ATTRIBUTE_RO_FUNCTION("combinedRestitution", pyattr_get_combined_restitution),
	EXP_ATTRIBUTE_RO_FUNCTION("appliedImpulse", pyattr_get_applied_impulse),
	EXP_ATTRIBUTE_NULL //Sentinel
};

mt::vec3 KX_CollisionContactPoint::pyattr_get_local_point_a()
{
	return m_collData->GetLocalPointA(m_index, m_firstObject);
}

mt::vec3 KX_CollisionContactPoint::pyattr_get_local_point_b()
{
	return m_collData->GetLocalPointB(m_index, m_firstObject);
}

mt::vec3 KX_CollisionContactPoint::pyattr_get_world_point()
{
	return m_collData->GetWorldPoint(m_index, m_firstObject);
}

mt::vec3 KX_CollisionContactPoint::pyattr_get_normal()
{
	return m_collData->GetNormal(m_index, m_firstObject);
}

float KX_CollisionContactPoint::pyattr_get_combined_friction()
{
	return m_collData->GetCombinedFriction(m_index, m_firstObject);
}

float KX_CollisionContactPoint::pyattr_get_combined_rolling_friction()
{
	return m_collData->GetCombinedRollingFriction(m_index, m_firstObject);
}

float KX_CollisionContactPoint::pyattr_get_combined_restitution()
{
	return m_collData->GetCombinedRestitution(m_index, m_firstObject);
}

float KX_CollisionContactPoint::pyattr_get_applied_impulse()
{
	return m_collData->GetAppliedImpulse(m_index, m_firstObject);
}

static unsigned int kx_collision_contact_point_list_get_size_cb(EXP_PyObjectPlus *self_v)
{
	return ((KX_CollisionContactPointList *)self_v)->GetNumCollisionContactPoint();
}

static PyObject *kx_collision_contact_point_list_get_item_cb(EXP_PyObjectPlus *self_v, unsigned int index)
{
	return ((KX_CollisionContactPointList *)self_v)->GetCollisionContactPoint(index)->NewProxy(true);
}

#endif  // WITH_PYTHON

KX_CollisionContactPointList::KX_CollisionContactPointList(const PHY_ICollData *collData, bool firstObject)
	:
#ifdef WITH_PYTHON
	EXP_BaseListWrapper(this, kx_collision_contact_point_list_get_size_cb,
	                kx_collision_contact_point_list_get_item_cb, nullptr, nullptr, EXP_BaseListWrapper::FLAG_NO_WEAK_REF),
#endif  // WITH_PYTHON
	m_collData(collData),
	m_firstObject(firstObject)
{
}

KX_CollisionContactPointList::~KX_CollisionContactPointList()
{
}

std::string KX_CollisionContactPointList::GetName() const
{
	return "KX_CollisionContactPointList";
}

KX_CollisionContactPoint *KX_CollisionContactPointList::GetCollisionContactPoint(unsigned int index)
{
	// All contact point infos.
	return (new KX_CollisionContactPoint(m_collData, index, m_firstObject));
}

unsigned int KX_CollisionContactPointList::GetNumCollisionContactPoint()
{
	return m_collData->GetNumContacts();
}

const PHY_ICollData *KX_CollisionContactPointList::GetCollData()
{
	return m_collData;
}

bool KX_CollisionContactPointList::GetFirstObject()
{
	return m_firstObject;
}
