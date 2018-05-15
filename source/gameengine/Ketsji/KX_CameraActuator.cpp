/*
 * KX_CameraActuator.cpp
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
 *
 */

/** \file gameengine/Ketsji/KX_CameraActuator.cpp
 *  \ingroup ketsji
 */

#include "KX_CameraActuator.h"
#include "KX_GameObject.h"

/* ------------------------------------------------------------------------- */
/* Native functions                                                          */
/* ------------------------------------------------------------------------- */

KX_CameraActuator::KX_CameraActuator(SCA_IObject *gameobj,
                                     SCA_IObject *obj,
                                     float hght,
                                     float minhght,
                                     float maxhght,
                                     short axis,
                                     float damping) :
	SCA_IActuator(gameobj, KX_ACT_CAMERA),
	m_ob(obj),
	m_height(hght),
	m_minHeight(minhght),
	m_maxHeight(maxhght),
	m_axis(axis),
	m_damping(damping)
{
	if (m_ob) {
		m_ob->RegisterActuator(this);
	}
}

KX_CameraActuator::~KX_CameraActuator()
{
	if (m_ob) {
		m_ob->UnregisterActuator(this);
	}
}

EXP_Value *KX_CameraActuator::GetReplica()
{
	KX_CameraActuator *replica = new KX_CameraActuator(*this);
	replica->ProcessReplica();
	return replica;
};

void KX_CameraActuator::ProcessReplica()
{
	if (m_ob) {
		m_ob->RegisterActuator(this);
	}
	SCA_IActuator::ProcessReplica();
}

bool KX_CameraActuator::UnlinkObject(SCA_IObject *clientobj)
{
	if (clientobj == m_ob) {
		// this object is being deleted, we cannot continue to track it.
		m_ob = nullptr;
		return true;
	}
	return false;
}


void KX_CameraActuator::Relink(std::map<SCA_IObject *, SCA_IObject *>& obj_map)
{
	SCA_IObject *obj = obj_map[m_ob];
	if (obj) {
		if (m_ob) {
			m_ob->UnregisterActuator(this);
		}
		m_ob = obj;
		m_ob->RegisterActuator(this);
	}
}

/* copied from blender BLI_math ... don't know if there's an equivalent */

static void Kx_VecUpMat3(mt::vec3 &vec, mt::mat3& mat, short axis)
{
	// Construct a camera matrix s.t. the specified axis

	// maps to the given vector (*vec). Also defines the rotation

	// about this axis by mapping one of the other axis to the y-axis.


	float inp;
	short cox = 0, coy = 0, coz = 0;

	/* up range has no meaning, is not really up!
	 * see: VecUpMat3old
	 */

	if (axis == 0) {
		cox = 0; coy = 1; coz = 2;     /* Y up Z tr */
	}
	if (axis == 1) {
		cox = 1; coy = 2; coz = 0;     /* Z up X tr */
	}
	if (axis == 2) {
		cox = 2; coy = 0; coz = 1;     /* X up Y tr */
	}
	if (axis == 3) {
		cox = 0; coy = 1; coz = 2;     /* Y op -Z tr */
		vec = -vec;
	}
	if (axis == 4) {
		cox = 1; coy = 0; coz = 2;     /*  */
	}
	if (axis == 5) {
		cox = 2; coy = 1; coz = 0;     /* Y up X tr */
	}

	mat.GetColumn(coz) = vec;
	if (mat.GetColumn(coz).Normalize() == 0.f) {
		/* this is a very abnormal situation: the camera has reach the object center exactly
		 * We will choose a completely arbitrary direction */
		mat.GetColumn(coz) = mt::axisX3;
	}

	inp = mat(coz, 2);
	mat(0, coy) =      -inp *mat(0, coz);
	mat(1, coy) =      -inp *mat(1, coz);
	mat(2, coy) = 1.0f - inp * mat(2, coz);

	if (mat.GetColumn(coy).Normalize() == 0.f) {
		/* the camera is vertical, chose the y axis arbitrary */
		mat.GetColumn(coy) = mt::axisY3;
	}

	mat.GetColumn(cox) = mt::vec3::CrossProduct(mat.GetColumn(coy), mat.GetColumn(coz));
}

bool KX_CameraActuator::Update(double curtime)
{
	/* wondering... is it really necessary/desirable to suppress negative    */
	/* events here?                                                          */
	bool bNegativeEvent = IsNegativeEvent();
	RemoveAllEvents();

	if (bNegativeEvent || !m_ob) {
		return false;
	}

	KX_GameObject *obj = (KX_GameObject *)GetParent();
	mt::vec3 from = obj->NodeGetWorldPosition();
	mt::mat3 frommat = obj->NodeGetWorldOrientation();
	/* These casts are _very_ dangerous!!! */
	mt::vec3 lookat = ((KX_GameObject *)m_ob)->NodeGetWorldPosition();
	mt::mat3 actormat = ((KX_GameObject *)m_ob)->NodeGetWorldOrientation();

	mt::vec3 fp1, fp2, rc;
	float inp, fac; //, factor = 0.0; /* some factor...                                    */
	float mindistsq, maxdistsq, distsq;
	mt::mat3 mat;

	/* The rules:                                                            */
	/* CONSTRAINT 1: not implemented */
	/* CONSTRAINT 2: can camera see actor?              */
	/* CONSTRAINT 3: fixed height relative to floor below actor.             */
	/* CONSTRAINT 4: camera rotates behind actor                              */
	/* CONSTRAINT 5: minimum / maximum distance                             */
	/* CONSTRAINT 6: again: fixed height relative to floor below actor        */
	/* CONSTRAINT 7: track to floor below actor                               */
	/* CONSTRAINT 8: look a little bit left or right, depending on how the
	 *
	 * character is looking (horizontal x)
	 */

	/* ...and then set the camera position. Since we assume the parent of    */
	/* this actuator is always a camera, just set the parent position and    */
	/* rotation. We do not check whether we really have a camera as parent.  */
	/* It may be better to turn this into a general tracking actuator later  */
	/* on, since lots of plausible relations can be filled in here.          */

	/* ... set up some parameters ...                                        */
	/* missing here: the 'floorloc' of the actor's shadow */

	mindistsq = m_minHeight * m_minHeight;
	maxdistsq = m_maxHeight * m_maxHeight;

	/* C1: not checked... is a future option                                 */

	/* C2: blender test_visibility function. Can this be a ray-test?         */

	/* C3: fixed height  */
	from[2] = (15.0f * from[2] + lookat[2] + m_height) / 16.0f;


	/* C4: camera behind actor   */
	switch (m_axis) {
		case OB_POSX:
		{
			/* X */
			fp1 = actormat.GetColumn(0);

			fp2 = frommat.GetColumn(0);
			break;
		}
		case OB_POSY:
		{
			/* Y */
			fp1 = actormat.GetColumn(1);

			fp2 = frommat.GetColumn(1);
			break;
		}
		case OB_NEGX:
		{
			/* -X */
			fp1 = -actormat.GetColumn(0);

			fp2 = frommat.GetColumn(0);
			break;
		}
		case OB_NEGY:
		{
			/* -Y */
			fp1 = -actormat.GetColumn(1);

			fp2 = frommat.GetColumn(1);
			break;
		}
		default:
		{
			BLI_assert(0);
			break;
		}
	}

	inp = mt::vec3::DotProduct(fp1, fp2);
	fac = (-1.0f + inp) * m_damping;

	from += fac * fp1;

	/* only for it lies: cross test and perpendicular bites up */
	if (inp < 0.0f) {
		/* Don't do anything if the cross product is too small.
		 * The camera up-axis becomes unstable and starts to oscillate.
		 * The 0.01f threshold is arbitrary but seems to work well in practice. */
		float cross = fp1[0] * fp2[1] - fp1[1] * fp2[0];
		if (cross > 0.01f) {
			from[0] -= fac * fp1[1];
			from[1] += fac * fp1[0];
		}
		else if (cross < -0.01f) {
			from[0] += fac * fp1[1];
			from[1] -= fac * fp1[0];
		}
	}

	/* CONSTRAINT 5: minimum / maximum distance */

	rc = lookat - from;
	distsq = rc.LengthSquared();

	if (distsq > maxdistsq) {
		distsq = 0.15f * (distsq - maxdistsq) / distsq;

		from += distsq * rc;
	}
	else if (distsq < mindistsq) {
		distsq = 0.15f * (mindistsq - distsq) / mindistsq;

		from -= distsq * rc;
	}

	/* CONSTRAINT 7: track to floor below actor */
	rc = lookat - from;
	Kx_VecUpMat3(rc, mat, 3);   /* y up Track -z */

	/* now set the camera position and rotation */

	obj->NodeSetLocalPosition(from);

	obj->NodeSetLocalOrientation(mat);

	return true;
}

#ifdef WITH_PYTHON

/* ------------------------------------------------------------------------- */
/* Python functions                                                          */
/* ------------------------------------------------------------------------- */

/* Integration hooks ------------------------------------------------------- */
PyTypeObject KX_CameraActuator::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"KX_CameraActuator",
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

PyMethodDef KX_CameraActuator::Methods[] = {
	{nullptr, nullptr} //Sentinel
};

PyAttributeDef KX_CameraActuator::Attributes[] = {
	EXP_PYATTRIBUTE_FLOAT_RW("min", -FLT_MAX, FLT_MAX, KX_CameraActuator, m_minHeight),
	EXP_PYATTRIBUTE_FLOAT_RW("max", -FLT_MAX, FLT_MAX, KX_CameraActuator, m_maxHeight),
	EXP_PYATTRIBUTE_FLOAT_RW("height", -FLT_MAX, FLT_MAX, KX_CameraActuator, m_height),
	EXP_PYATTRIBUTE_SHORT_RW("axis", 0, 5, true, KX_CameraActuator, m_axis),
	EXP_PYATTRIBUTE_RW_FUNCTION("object", KX_CameraActuator, pyattr_get_object, pyattr_set_object),
	EXP_PYATTRIBUTE_FLOAT_RW("damping", 0.f, 10.f, KX_CameraActuator, m_damping),
	EXP_PYATTRIBUTE_NULL
};

PyObject *KX_CameraActuator::pyattr_get_object(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_CameraActuator *self = static_cast<KX_CameraActuator *>(self_v);
	if (self->m_ob == nullptr) {
		Py_RETURN_NONE;
	}
	else {
		return self->m_ob->GetProxy();
	}
}

int KX_CameraActuator::pyattr_set_object(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_CameraActuator *self = static_cast<KX_CameraActuator *>(self_v);
	KX_GameObject *gameobj;

	if (!ConvertPythonToGameObject(self->GetLogicManager(), value, &gameobj, true, "actuator.object = value: KX_CameraActuator")) {
		return PY_SET_ATTR_FAIL; // ConvertPythonToGameObject sets the error

	}
	if (self->m_ob) {
		self->m_ob->UnregisterActuator(self);
	}

	if ((self->m_ob = (SCA_IObject *)gameobj)) {
		self->m_ob->RegisterActuator(self);
	}

	return PY_SET_ATTR_SUCCESS;
}

#endif // WITH_PYTHON

/* eof */
