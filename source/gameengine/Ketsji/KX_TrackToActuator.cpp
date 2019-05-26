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

/** \file gameengine/Ketsji/KX_TrackToActuator.cpp
 *  \ingroup ketsji
 *
 * Replace the mesh for this actuator's parent
 */

/* m_trackflag is used to determine the forward tracking direction
 * m_upflag for the up direction
 * normal situation is +y for forward, +z for up */

#include "SCA_IActuator.h"
#include "KX_TrackToActuator.h"
#include "SCA_IScene.h"
#include "SCA_LogicManager.h"
#include <math.h>
#include <iostream>
#include "KX_GameObject.h"

#include "EXP_PyObjectPlus.h"

/* ------------------------------------------------------------------------- */
/* Native functions                                                          */
/* ------------------------------------------------------------------------- */

KX_TrackToActuator::KX_TrackToActuator(SCA_IObject *gameobj,
                                       SCA_IObject *ob,
                                       int time,
                                       bool allow3D,
                                       int trackflag,
                                       int upflag)
	:SCA_IActuator(gameobj, KX_ACT_TRACKTO)
{
	m_time = time;
	m_allow3D = allow3D;
	m_object = ob;
	m_trackflag = trackflag;
	m_upflag = upflag;
	m_parentobj = 0;

	if (m_object) {
		m_object->RegisterActuator(this);
	}

	{
		// if the object is vertex parented, don't check parent orientation as the link is broken
		if (!((KX_GameObject *)gameobj)->IsVertexParent()) {
			m_parentobj = ((KX_GameObject *)gameobj)->GetParent(); // check if the object is parented
			if (m_parentobj) {
				// if so, store the initial local rotation
				// this is needed to revert the effect of the parent inverse node (TBC)
				m_parentlocalmat = m_parentobj->GetNode()->GetLocalOrientation();
				// use registration mechanism rather than AddRef, it creates zombie objects
				m_parentobj->RegisterActuator(this);
			}
		}
	}

} /* End of constructor */



/* old function from Blender */
static mt::mat3 EulToMat3(float eul[3])
{
	mt::mat3 mat;
	float ci, cj, ch, si, sj, sh, cc, cs, sc, ss;

	ci = cosf(eul[0]);
	cj = cosf(eul[1]);
	ch = cosf(eul[2]);
	si = sinf(eul[0]);
	sj = sinf(eul[1]);
	sh = sinf(eul[2]);
	cc = ci * ch;
	cs = ci * sh;
	sc = si * ch;
	ss = si * sh;

	mat(0, 0) = cj * ch;
	mat(0, 1) = sj * sc - cs;
	mat(0, 2) = sj * cc + ss;
	mat(1, 0) = cj * sh;
	mat(1, 1) = sj * ss + cc;
	mat(1, 2) = sj * cs - sc;
	mat(2, 0) = -sj;
	mat(2, 1) = cj * si;
	mat(2, 2) = cj * ci;

	return mat;
}



/* old function from Blender */
static void Mat3ToEulOld(const mt::mat3 &mat, float eul[3])
{
	const float cy = sqrtf(mat(0, 0) * mat(0, 0) + mat(1, 0) * mat(1, 0));

	if (cy > (float)(16.0f * FLT_EPSILON)) {
		eul[0] = atan2f(mat(2, 1), mat(2, 2));
		eul[1] = atan2f(-mat(2, 0), cy);
		eul[2] = atan2f(mat(1, 0), mat(0, 0));
	}
	else {
		eul[0] = atan2f(-mat(1, 2), mat(1, 1));
		eul[1] = atan2f(-mat(2, 0), cy);
		eul[2] = 0.0f;
	}
}



/* old function from Blender */
static void compatible_eulFast(float *eul, float *oldrot)
{
	float dx, dy, dz;

	/* angular difference of 360 degrees */

	dx = eul[0] - oldrot[0];
	dy = eul[1] - oldrot[1];
	dz = eul[2] - oldrot[2];

	if (fabsf(dx) > (float)M_PI) {
		if (dx > 0.0f) {
			eul[0] -= (float)(M_PI * 2.0f);
		}
		else {
			eul[0] += (float)(M_PI * 2.0f);
		}
	}
	if (fabsf(dy) > (float)M_PI) {
		if (dy > 0.0f) {
			eul[1] -= (float)(M_PI * 2.0f);
		}
		else {
			eul[1] += (float)(M_PI * 2.0f);
		}
	}
	if (fabsf(dz) > (float)M_PI) {
		if (dz > 0.0f) {
			eul[2] -= (float)(M_PI * 2.0f);
		}
		else {
			eul[2] += (float)(M_PI * 2.0f);
		}
	}
}



static mt::mat3 matrix3x3_interpol(const mt::mat3 &oldmat, const mt::mat3 &mat, int m_time)
{
	float eul[3], oldeul[3];

	Mat3ToEulOld(oldmat, oldeul);
	Mat3ToEulOld(mat, eul);
	compatible_eulFast(eul, oldeul);

	eul[0] = (m_time * oldeul[0] + eul[0]) / (1.0f + m_time);
	eul[1] = (m_time * oldeul[1] + eul[1]) / (1.0f + m_time);
	eul[2] = (m_time * oldeul[2] + eul[2]) / (1.0f + m_time);

	return EulToMat3(eul);
}

static float basis_cross(int n, int m)
{
	switch (n - m) {
		case 1:
		case -2:
		{
			return 1.0f;

		}
		case -1:
		case 2:
		{
			return -1.0f;

		}
		default:
			return 0.0f;
	}
}

/* vectomat function obtained from constrain.c and modified to work with MOTO library */
static mt::mat3 vectomat(const mt::vec3 &dir, short axis, short upflag, short threedimup)
{
	mt::mat3 mat;
	mt::vec3 proj;
	mt::vec3 right;
	float mul;
	int right_index;

	/* Normalized Vec vector*/
	mt::vec3 vec = dir.SafeNormalized(mt::axisZ3);

	/* if 2D doesn't move the up vector */
	if (!threedimup) {
		vec.z = 0.0f;
		vec = (vec - mt::dot(mt::axisZ3, vec) * mt::axisZ3).SafeNormalized(mt::axisZ3);
	}

	if (axis > 2) {
		axis -= 3;
	}
	else {
		vec = -vec;
	}

	/* project the up vector onto the plane specified by vec */
	/* first z onto vec... */
	mul = mt::dot(mt::axisZ3, vec) / mt::dot(vec, vec);
	proj = vec * mul;
	/* then onto the plane */
	proj = mt::axisZ3 - proj;
	/* proj specifies the transformation of the up axis */
	proj = proj.SafeNormalized(mt::axisY3);

	/* Normalized cross product of vec and proj specifies transformation of the right axis */
	right = mt::cross(proj, vec);
	right.Normalize();

	if (axis != upflag) {
		right_index = 3 - axis - upflag;

		/* account for up direction, track direction */
		right = right * basis_cross(axis, upflag);

		mat.GetColumn(right_index) = right;
		mat.GetColumn(upflag) = proj;
		mat.GetColumn(axis) = vec;
	}
	/* identity matrix - don't do anything if the two axes are the same */
	else {
		mat = mt::mat3::Identity();
	}

	return mat;
}

KX_TrackToActuator::~KX_TrackToActuator()
{
	if (m_object) {
		m_object->UnregisterActuator(this);
	}
	if (m_parentobj) {
		m_parentobj->UnregisterActuator(this);
	}
} /* end of destructor */

void KX_TrackToActuator::ProcessReplica()
{
	// the replica is tracking the same object => register it
	if (m_object) {
		m_object->RegisterActuator(this);
	}
	if (m_parentobj) {
		m_parentobj->RegisterActuator(this);
	}
	SCA_IActuator::ProcessReplica();
}


bool KX_TrackToActuator::UnlinkObject(SCA_IObject *clientobj)
{
	if (clientobj == m_object) {
		// this object is being deleted, we cannot continue to track it.
		m_object = nullptr;
		return true;
	}
	if (clientobj == m_parentobj) {
		m_parentobj = nullptr;
		return true;
	}
	return false;
}

void KX_TrackToActuator::Relink(std::map<SCA_IObject *, SCA_IObject *>& obj_map)
{
	SCA_IObject *obj = obj_map[m_object];
	if (obj) {
		if (m_object) {
			m_object->UnregisterActuator(this);
		}
		m_object = obj;
		m_object->RegisterActuator(this);
	}

	KX_GameObject *parobj = static_cast<KX_GameObject *>(obj_map[m_parentobj]);
	if (parobj) {
		if (m_parentobj) {
			m_parentobj->UnregisterActuator(this);
		}
		m_parentobj = parobj;
		m_parentobj->RegisterActuator(this);
	}
}


bool KX_TrackToActuator::Update(double curtime)
{
	bool result = false;
	bool bNegativeEvent = IsNegativeEvent();
	RemoveAllEvents();

	if (bNegativeEvent) {
		// do nothing on negative events
	}
	else if (m_object) {
		KX_GameObject *curobj = (KX_GameObject *)GetParent();
		mt::vec3 dir = curobj->NodeGetWorldPosition() - ((KX_GameObject *)m_object)->NodeGetWorldPosition();
		mt::mat3 mat;
		mt::mat3 oldmat;

		mat = vectomat(dir, m_trackflag, m_upflag, m_allow3D);
		oldmat = curobj->NodeGetWorldOrientation();

		/* erwin should rewrite this! */
		mat = matrix3x3_interpol(oldmat, mat, m_time);

		/* check if the model is parented and calculate the child transform */
		if (m_parentobj) {

			mt::vec3 localpos;
			localpos = curobj->GetNode()->GetLocalPosition();
			// Get the inverse of the parent matrix
			mt::mat3 parentmatinv;
			parentmatinv = m_parentobj->NodeGetWorldOrientation().Inverse();
			// transform the local coordinate system into the parents system
			mat = parentmatinv * mat;
			// append the initial parent local rotation matrix
			mat = m_parentlocalmat * mat;

			// set the models tranformation properties
			curobj->NodeSetLocalOrientation(mat);
			curobj->NodeSetLocalPosition(localpos);
			//curobj->UpdateTransform();
		}
		else {
			curobj->NodeSetLocalOrientation(mat);
		}

		result = true;
	}

	return result;
}

#ifdef WITH_PYTHON

/* ------------------------------------------------------------------------- */
/* Python functions                                                          */
/* ------------------------------------------------------------------------- */

/* Integration hooks ------------------------------------------------------- */
PyTypeObject KX_TrackToActuator::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"KX_TrackToActuator",
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

PyMethodDef KX_TrackToActuator::Methods[] = {
	{nullptr, nullptr} //Sentinel
};

PyAttributeDef KX_TrackToActuator::Attributes[] = {
	EXP_PYATTRIBUTE_INT_RW("time", 0, 1000, true, KX_TrackToActuator, m_time),
	EXP_PYATTRIBUTE_BOOL_RW("use3D", KX_TrackToActuator, m_allow3D),
	EXP_PYATTRIBUTE_INT_RW("upAxis", 0, 2, true, KX_TrackToActuator, m_upflag),
	EXP_PYATTRIBUTE_INT_RW("trackAxis", 0, 5, true, KX_TrackToActuator, m_trackflag),
	EXP_PYATTRIBUTE_RW_FUNCTION("object", KX_TrackToActuator, pyattr_get_object, pyattr_set_object),

	EXP_PYATTRIBUTE_NULL    //Sentinel
};

PyObject *KX_TrackToActuator::pyattr_get_object(EXP_PyObjectPlus *self, const struct EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_TrackToActuator *actuator = static_cast<KX_TrackToActuator *>(self);
	if (!actuator->m_object) {
		Py_RETURN_NONE;
	}
	else {
		return actuator->m_object->GetProxy();
	}
}

int KX_TrackToActuator::pyattr_set_object(EXP_PyObjectPlus *self, const struct EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_TrackToActuator *actuator = static_cast<KX_TrackToActuator *>(self);
	KX_GameObject *gameobj;

	if (!ConvertPythonToGameObject(actuator->GetLogicManager(), value, &gameobj, true, "actuator.object = value: KX_TrackToActuator")) {
		return PY_SET_ATTR_FAIL; // ConvertPythonToGameObject sets the error

	}
	if (actuator->m_object != nullptr) {
		actuator->m_object->UnregisterActuator(actuator);
	}

	actuator->m_object = (SCA_IObject *)gameobj;

	if (actuator->m_object) {
		actuator->m_object->RegisterActuator(actuator);
	}

	return PY_SET_ATTR_SUCCESS;
}

#endif // WITH_PYTHON

/* eof */
