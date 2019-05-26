/*
 * Apply a constraint to a position or rotation value
 *
 *
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

/** \file gameengine/Ketsji/KX_ConstraintActuator.cpp
 *  \ingroup ketsji
 */


#include "SCA_IActuator.h"
#include "KX_ConstraintActuator.h"
#include "SCA_IObject.h"
#include "KX_GameObject.h"
#include "KX_RayCast.h"
#include "KX_Globals.h" // KX_GetActiveScene
#include "KX_Mesh.h"

#include "CM_Message.h"

/* ------------------------------------------------------------------------- */
/* Native functions                                                          */
/* ------------------------------------------------------------------------- */

KX_ConstraintActuator::KX_ConstraintActuator(SCA_IObject *gameobj,
                                             int posDampTime,
                                             int rotDampTime,
                                             float minBound,
                                             float maxBound,
                                             const mt::vec3& refDir,
                                             int locrotxyz,
                                             int time,
                                             int option,
                                             char *property) :
	SCA_IActuator(gameobj, KX_ACT_CONSTRAINT),
	m_refDirection(refDir),
	m_currentTime(0)
{
	m_posDampTime = posDampTime;
	m_rotDampTime = rotDampTime;
	m_locrot   = locrotxyz;
	m_option = option;
	m_activeTime = time;
	if (property) {
		m_property = property;
	}
	else {
		m_property = "";
	}
	/* The units of bounds are determined by the type of constraint. To      */
	/* make the constraint application easier and more transparent later on, */
	/* I think converting the bounds to the applicable domain makes more     */
	/* sense.                                                                */
	switch (m_locrot) {
		case KX_ACT_CONSTRAINT_ORIX:
		case KX_ACT_CONSTRAINT_ORIY:
		case KX_ACT_CONSTRAINT_ORIZ:
		{
			float len = m_refDirection.Length();
			if (mt::FuzzyZero(len)) {
				// missing a valid direction
				CM_LogicBrickWarning(this, "there is no valid reference direction!");
				m_locrot = KX_ACT_CONSTRAINT_NODEF;
			}
			else {
				m_refDirection /= len;
			}
			m_minimumBound = cosf(minBound);
			m_maximumBound = cosf(maxBound);
			m_minimumSine = sinf(minBound);
			m_maximumSine = sinf(maxBound);
			break;
		}
		default:
		{
			m_minimumBound = minBound;
			m_maximumBound = maxBound;
			m_minimumSine = 0.f;
			m_maximumSine = 0.f;
			break;
		}
	}

} /* End of constructor */

KX_ConstraintActuator::~KX_ConstraintActuator()
{
	// there's nothing to be done here, really....
} /* end of destructor */

bool KX_ConstraintActuator::RayHit(KX_ClientObjectInfo *client, KX_RayCast *result, void *UNUSED(data))
{

	m_hitObject = client->m_gameobject;

	bool bFound = false;

	if (m_property.empty()) {
		bFound = true;
	}
	else {
		if (m_option & KX_ACT_CONSTRAINT_MATERIAL) {
			for (KX_Mesh *meshObj : m_hitObject->GetMeshList()) {
				bFound = (meshObj->FindMaterialName(m_property) != nullptr);
				if (bFound) {
					break;
				}
			}
		}
		else {
			bFound = m_hitObject->GetProperty(m_property) != nullptr;
		}
	}
	// update the hit status
	result->m_hitFound = bFound;
	// stop looking
	return true;
}

/* This function is used to pre-filter the object before casting the ray on them.
 * This is useful for "X-Ray" option when we want to see "through" unwanted object.
 */
bool KX_ConstraintActuator::NeedRayCast(KX_ClientObjectInfo *client, void *UNUSED(data))
{
	if (client->m_type > KX_ClientObjectInfo::ACTOR) {
		// Unknown type of object, skip it.
		// Should not occur as the sensor objects are filtered in RayTest()
		CM_LogicBrickError(this, "invalid client type " << client->m_type << " found in ray casting");
		return false;
	}
	// no X-Ray function yet
	return true;
}

bool KX_ConstraintActuator::Update(double curtime)
{

	bool result = false;
	bool bNegativeEvent = IsNegativeEvent();
	RemoveAllEvents();

	if (!bNegativeEvent) {
		/* Constraint clamps the values to the specified range, with a sort of    */
		/* low-pass filtered time response, if the damp time is unequal to 0.     */

		/* Having to retrieve location/rotation and setting it afterwards may not */
		/* be efficient enough... Something to look at later.                     */
		KX_GameObject  *obj = (KX_GameObject *)GetParent();
		mt::vec3 position = obj->NodeGetWorldPosition();
		mt::vec3 newposition;
		mt::vec3 normal, direction, refDirection;
		mt::mat3 rotation = obj->NodeGetWorldOrientation();
		float filter, newdistance, cosangle;
		int axis, sign;

		if (m_posDampTime) {
			filter = m_posDampTime / (1.0f + m_posDampTime);
		}
		else {
			filter = 0.0f;
		}
		switch (m_locrot) {
			case KX_ACT_CONSTRAINT_ORIX:
			case KX_ACT_CONSTRAINT_ORIY:
			case KX_ACT_CONSTRAINT_ORIZ:
			{
				switch (m_locrot) {
					case KX_ACT_CONSTRAINT_ORIX:
					{
						direction = rotation.GetColumn(0);
						axis = 0;
						break;
					}
					case KX_ACT_CONSTRAINT_ORIY:
					{
						direction = rotation.GetColumn(1);
						axis = 1;
						break;
					}
					default:
					{
						direction = rotation.GetColumn(2);
						axis = 2;
						break;
					}
				}
				if ((m_maximumBound < (1.0f - FLT_EPSILON)) || (m_minimumBound < (1.0f - FLT_EPSILON))) {
					// reference direction needs to be evaluated
					// 1. get the cosine between current direction and target
					cosangle = mt::dot(direction, m_refDirection);
					if (cosangle >= (m_maximumBound - FLT_EPSILON) && cosangle <= (m_minimumBound + FLT_EPSILON)) {
						// no change to do
						result = true;
						goto CHECK_TIME;
					}
					// 2. define a new reference direction
					//    compute local axis with reference direction as X and
					//    Y in direction X refDirection plane
					mt::vec3 zaxis = mt::cross(m_refDirection, direction);
					if (mt::FuzzyZero(zaxis.LengthSquared())) {
						// direction and refDirection are identical,
						// choose any other direction to define plane
						if (direction[0] < 0.9999f) {
							zaxis = mt::cross(m_refDirection, mt::axisX3);
						}
						else {
							zaxis = mt::cross(m_refDirection, mt::axisY3);
						}
					}
					mt::vec3 yaxis = mt::cross(zaxis, m_refDirection);
					yaxis.Normalize();
					if (cosangle > m_minimumBound) {
						// angle is too close to reference direction,
						// choose a new reference that is exactly at minimum angle
						refDirection = m_minimumBound * m_refDirection + m_minimumSine * yaxis;
					}
					else {
						// angle is too large, choose new reference direction at maximum angle
						refDirection = m_maximumBound * m_refDirection + m_maximumSine * yaxis;
					}
				}
				else {
					refDirection = m_refDirection;
				}
				// apply damping on the direction
				direction = filter * direction + (1.0f - filter) * refDirection;
				obj->AlignAxisToVect(direction, axis);
				result = true;
				goto CHECK_TIME;
			}
			case KX_ACT_CONSTRAINT_DIRPX:
			case KX_ACT_CONSTRAINT_DIRPY:
			case KX_ACT_CONSTRAINT_DIRPZ:
			case KX_ACT_CONSTRAINT_DIRNX:
			case KX_ACT_CONSTRAINT_DIRNY:
			case KX_ACT_CONSTRAINT_DIRNZ:
			{
				switch (m_locrot) {
					case KX_ACT_CONSTRAINT_DIRPX:
					{
						normal = rotation.GetColumn(0);
						axis = 0; // axis according to KX_GameObject::AlignAxisToVect()
						sign = 0; // X axis will be parrallel to direction of ray
						break;
					}
					case KX_ACT_CONSTRAINT_DIRPY:
					{
						normal = rotation.GetColumn(1);
						axis = 1;
						sign = 0;
						break;
					}
					case KX_ACT_CONSTRAINT_DIRPZ:
					{
						normal = rotation.GetColumn(2);
						axis = 2;
						sign = 0;
						break;
					}
					case KX_ACT_CONSTRAINT_DIRNX:
					{
						normal = -rotation.GetColumn(0);
						axis = 0;
						sign = 1;
						break;
					}
					case KX_ACT_CONSTRAINT_DIRNY:
					{
						normal = -rotation.GetColumn(1);
						axis = 1;
						sign = 1;
						break;
					}
					case KX_ACT_CONSTRAINT_DIRNZ:
					{
						normal = -rotation.GetColumn(2);
						axis = 2;
						sign = 1;
						break;
					}
				}
				normal.Normalize();
				if (m_option & KX_ACT_CONSTRAINT_LOCAL) {
					// direction of the ray is along the local axis
					direction = normal;
				}
				else {
					switch (m_locrot) {
						case KX_ACT_CONSTRAINT_DIRPX:
						{
							direction = mt::axisX3;
							break;
						}
						case KX_ACT_CONSTRAINT_DIRPY:
						{
							direction = mt::axisY3;
							break;
						}
						case KX_ACT_CONSTRAINT_DIRPZ:
						{
							direction = mt::axisZ3;
							break;
						}
						case KX_ACT_CONSTRAINT_DIRNX:
						{
							direction = -mt::axisX3;
							break;
						}
						case KX_ACT_CONSTRAINT_DIRNY:
						{
							direction = -mt::axisY3;
							break;
						}
						case KX_ACT_CONSTRAINT_DIRNZ:
						{
							direction = -mt::axisZ3;
							break;
						}
					}
				}
				{
					mt::vec3 topoint = position + (m_maximumBound) * direction;
					PHY_IPhysicsEnvironment *pe = KX_GetActiveScene()->GetPhysicsEnvironment();
					PHY_IPhysicsController *spc = obj->GetPhysicsController();

					if (!pe) {
						CM_LogicBrickWarning(this, "there is no physics environment!");
						goto CHECK_TIME;
					}
					if (!spc) {
						// the object is not physical, we probably want to avoid hitting its own parent
						KX_GameObject *parent = obj->GetParent();
						if (parent) {
							spc = parent->GetPhysicsController();
						}
					}
					KX_RayCast::Callback<KX_ConstraintActuator, void> callback(this, dynamic_cast<PHY_IPhysicsController *>(spc));
					result = KX_RayCast::RayTest(pe, position, topoint, callback);
					if (result) {
						mt::vec3 newnormal = callback.m_hitNormal;
						// compute new position & orientation
						if ((m_option & (KX_ACT_CONSTRAINT_NORMAL | KX_ACT_CONSTRAINT_DISTANCE)) == 0) {
							// if none option is set, the actuator does nothing but detect ray
							// (works like a sensor)
							goto CHECK_TIME;
						}
						if (m_option & KX_ACT_CONSTRAINT_NORMAL) {
							float rotFilter;
							// apply damping on the direction
							if (m_rotDampTime) {
								rotFilter = m_rotDampTime / (1.0f + m_rotDampTime);
							}
							else {
								rotFilter = filter;
							}
							newnormal = rotFilter * normal - (1.0f - rotFilter) * newnormal;
							obj->AlignAxisToVect((sign) ? -newnormal : newnormal, axis);
							if (m_option & KX_ACT_CONSTRAINT_LOCAL) {
								direction = newnormal;
								direction.Normalize();
							}
						}
						if (m_option & KX_ACT_CONSTRAINT_DISTANCE) {
							if (m_posDampTime) {
								newdistance = filter * (position - callback.m_hitPoint).Length() + (1.0f - filter) * m_minimumBound;
							}
							else {
								newdistance = m_minimumBound;
							}
							// logically we should cancel the speed along the ray direction as we set the
							// position along that axis
							spc = obj->GetPhysicsController();
							if (spc && spc->IsDynamic()) {
								mt::vec3 linV = spc->GetLinearVelocity();
								// cancel the projection along the ray direction
								float fallspeed = mt::dot(linV, direction);
								if (!mt::FuzzyZero(fallspeed)) {
									spc->SetLinearVelocity(linV - fallspeed * direction, false);
								}
							}
						}
						else {
							newdistance = (position - callback.m_hitPoint).Length();
						}
						newposition = callback.m_hitPoint - newdistance * direction;
					}
					else if (m_option & KX_ACT_CONSTRAINT_PERMANENT) {
						// no contact but still keep running
						result = true;
						goto CHECK_TIME;
					}
				}
				break;
			}
			case KX_ACT_CONSTRAINT_FHPX:
			case KX_ACT_CONSTRAINT_FHPY:
			case KX_ACT_CONSTRAINT_FHPZ:
			case KX_ACT_CONSTRAINT_FHNX:
			case KX_ACT_CONSTRAINT_FHNY:
			case KX_ACT_CONSTRAINT_FHNZ:
			{
				switch (m_locrot) {
					case KX_ACT_CONSTRAINT_FHPX:
					{
						normal = -rotation.GetColumn(0);
						direction = mt::axisX3;
						break;
					}
					case KX_ACT_CONSTRAINT_FHPY:
					{
						normal = -rotation.GetColumn(1);
						direction = mt::axisY3;
						break;
					}
					case KX_ACT_CONSTRAINT_FHPZ:
					{
						normal = -rotation.GetColumn(2);
						direction = mt::axisZ3;
						break;
					}
					case KX_ACT_CONSTRAINT_FHNX:
					{
						normal = rotation.GetColumn(0);
						direction = -mt::axisX3;
						break;
					}
					case KX_ACT_CONSTRAINT_FHNY:
					{
						normal = rotation.GetColumn(1);
						direction = -mt::axisY3;
						break;
					}
					case KX_ACT_CONSTRAINT_FHNZ:
					{
						normal = rotation.GetColumn(2);
						direction = -mt::axisZ3;
						break;
					}
				}
				normal.Normalize();
				{
					PHY_IPhysicsEnvironment *pe = KX_GetActiveScene()->GetPhysicsEnvironment();
					PHY_IPhysicsController *spc = obj->GetPhysicsController();

					if (!pe) {
						CM_LogicBrickWarning(this, "there is no physics environment!");
						goto CHECK_TIME;
					}
					if (!spc || !spc->IsDynamic()) {
						// the object is not dynamic, it won't support setting speed
						goto CHECK_TIME;
					}
					m_hitObject = nullptr;
					// distance of Fh area is stored in m_minimum
					mt::vec3 topoint = position + (m_minimumBound + spc->GetRadius()) * direction;
					KX_RayCast::Callback<KX_ConstraintActuator, void> callback(this, spc);
					result = KX_RayCast::RayTest(pe, position, topoint, callback);
					// we expect a hit object
					if (!m_hitObject) {
						result = false;
					}
					if (result) {
						mt::vec3 newnormal = callback.m_hitNormal;
						// compute new position & orientation
						float distance = (callback.m_hitPoint - position).Length() - spc->GetRadius();
						// estimate the velocity of the hit point
						mt::vec3 relativeHitPoint;
						relativeHitPoint = (callback.m_hitPoint - m_hitObject->NodeGetWorldPosition());
						mt::vec3 velocityHitPoint = m_hitObject->GetVelocity(relativeHitPoint);
						mt::vec3 relativeVelocity = spc->GetLinearVelocity() - velocityHitPoint;
						float relativeVelocityRay = mt::dot(direction, relativeVelocity);
						float springExtent = 1.0f - distance / m_minimumBound;
						// Fh force is stored in m_maximum
						float springForce = springExtent * m_maximumBound;
						// damping is stored in m_refDirection [0] = damping, [1] = rot damping
						float springDamp = relativeVelocityRay * m_refDirection[0];
						mt::vec3 newVelocity = spc->GetLinearVelocity() - (springForce + springDamp) * direction;
						if (m_option & KX_ACT_CONSTRAINT_NORMAL) {
							newVelocity += (springForce + springDamp) * (newnormal - mt::dot(newnormal, direction) * direction);
						}
						spc->SetLinearVelocity(newVelocity, false);
						if (m_option & KX_ACT_CONSTRAINT_DOROTFH) {
							mt::vec3 angSpring = (mt::cross(normal, newnormal)) * m_maximumBound;
							mt::vec3 angVelocity = spc->GetAngularVelocity();
							// remove component that is parallel to normal
							angVelocity -= mt::dot(angVelocity, newnormal) * newnormal;
							mt::vec3 angDamp = angVelocity * (mt::FuzzyZero(m_refDirection[1]) ? m_refDirection[0] : m_refDirection[1]);
							spc->SetAngularVelocity(spc->GetAngularVelocity() + (angSpring - angDamp), false);
						}
					}
					else if (m_option & KX_ACT_CONSTRAINT_PERMANENT) {
						// no contact but still keep running
						result = true;
					}
					// don't set the position with this constraint
					goto CHECK_TIME;
				}
				break;
			}
			case KX_ACT_CONSTRAINT_LOCX:
			case KX_ACT_CONSTRAINT_LOCY:
			case KX_ACT_CONSTRAINT_LOCZ:
				newposition = position = obj->GetNode()->GetLocalPosition();
				switch (m_locrot) {
					case KX_ACT_CONSTRAINT_LOCX:
					{
						CLAMP(newposition[0], m_minimumBound, m_maximumBound);
						break;
					}
					case KX_ACT_CONSTRAINT_LOCY:
					{
						CLAMP(newposition[1], m_minimumBound, m_maximumBound);
						break;
					}
					case KX_ACT_CONSTRAINT_LOCZ:
					{
						CLAMP(newposition[2], m_minimumBound, m_maximumBound);
						break;
					}
				}
				result = true;
				if (m_posDampTime) {
					newposition = filter * position + (1.0f - filter) * newposition;
				}
				obj->NodeSetLocalPosition(newposition);
				goto CHECK_TIME;
		}
		if (result) {
			// set the new position but take into account parent if any
			obj->NodeSetWorldPosition(newposition);
		}
CHECK_TIME:
		if (result && m_activeTime > 0) {
			if (++m_currentTime >= m_activeTime) {
				result = false;
			}
		}
	}
	if (!result) {
		m_currentTime = 0;
	}
	return result;
}

#ifdef WITH_PYTHON

/* ------------------------------------------------------------------------- */
/* Python functions                                                          */
/* ------------------------------------------------------------------------- */

/* Integration hooks ------------------------------------------------------- */
PyTypeObject KX_ConstraintActuator::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"KX_ConstraintActuator",
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
	&SCA_IActuator::Type,
	0, 0, 0, 0, 0, 0,
	py_base_new
};

PyMethodDef KX_ConstraintActuator::Methods[] = {
	{nullptr, nullptr} //Sentinel
};

PyAttributeDef KX_ConstraintActuator::Attributes[] = {
	EXP_PYATTRIBUTE_INT_RW("damp", 0, 100, true, KX_ConstraintActuator, m_posDampTime),
	EXP_PYATTRIBUTE_INT_RW("rotDamp", 0, 100, true, KX_ConstraintActuator, m_rotDampTime),
	EXP_PYATTRIBUTE_FLOAT_ARRAY_RW_CHECK("direction", -FLT_MAX, FLT_MAX, KX_ConstraintActuator, m_refDirection, 3, pyattr_check_direction),
	EXP_PYATTRIBUTE_INT_RW("option", 0, 0xFFFF, false, KX_ConstraintActuator, m_option),
	EXP_PYATTRIBUTE_INT_RW("time", 0, 1000, true, KX_ConstraintActuator, m_activeTime),
	EXP_PYATTRIBUTE_STRING_RW("propName", 0, MAX_PROP_NAME, true, KX_ConstraintActuator, m_property),
	EXP_PYATTRIBUTE_FLOAT_RW("min", -FLT_MAX, FLT_MAX, KX_ConstraintActuator, m_minimumBound),
	EXP_PYATTRIBUTE_FLOAT_RW("distance", -FLT_MAX, FLT_MAX, KX_ConstraintActuator, m_minimumBound),
	EXP_PYATTRIBUTE_FLOAT_RW("max", -FLT_MAX, FLT_MAX, KX_ConstraintActuator, m_maximumBound),
	EXP_PYATTRIBUTE_FLOAT_RW("rayLength", 0, 2000.f, KX_ConstraintActuator, m_maximumBound),
	EXP_PYATTRIBUTE_INT_RW("limit", KX_ConstraintActuator::KX_ACT_CONSTRAINT_NODEF + 1, KX_ConstraintActuator::KX_ACT_CONSTRAINT_MAX - 1, false, KX_ConstraintActuator, m_locrot),
	EXP_PYATTRIBUTE_NULL    //Sentinel
};

int KX_ConstraintActuator::pyattr_check_direction(EXP_PyObjectPlus *self_v, const struct EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_ConstraintActuator *act = static_cast<KX_ConstraintActuator *>(self_v);
	mt::vec3 dir(act->m_refDirection);
	float len = dir.Length();
	if (mt::FuzzyZero(len)) {
		PyErr_SetString(PyExc_ValueError, "actuator.direction = vec: KX_ConstraintActuator, invalid direction");
		return 1;
	}
	act->m_refDirection = dir / len;
	return 0;
}

#endif

/* eof */
