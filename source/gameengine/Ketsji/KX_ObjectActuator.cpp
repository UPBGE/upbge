/*
 * Do translation/rotation actions
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

/** \file gameengine/Ketsji/KX_ObjectActuator.cpp
 *  \ingroup ketsji
 */

#include "KX_ObjectActuator.h"
#include "KX_GameObject.h"
#include "KX_PyMath.h" // For PyVecTo.
#include "PHY_IPhysicsController.h"
#include "PHY_ICharacter.h"
#include "PHY_IPhysicsEnvironment.h"

#include "CM_Message.h"

KX_ObjectActuator::KX_ObjectActuator(SCA_IObject *gameobj,
                                     KX_GameObject *refobj,
                                     const mt::vec3& force,
                                     const mt::vec3& torque,
                                     const mt::vec3& dloc,
                                     const mt::vec3& drot,
                                     const mt::vec3& linV,
                                     const mt::vec3& angV,
                                     const short damping,
                                     const KX_LocalFlags& flag)
	:SCA_IActuator(gameobj, KX_ACT_OBJECT),
	m_force(force),
	m_torque(torque),
	m_dloc(dloc),
	m_drot(drot),
	m_linear_velocity(linV),
	m_angular_velocity(angV),
	m_linear_length2(0.0f),
	m_current_linear_factor(0.0f),
	m_current_angular_factor(0.0f),
	m_damping(damping),
	m_previous_error(0.0f, 0.0f, 0.0f),
	m_error_accumulator(0.0f, 0.0f, 0.0f),
	m_bitLocalFlag(flag),
	m_reference(refobj),
	m_linear_damping_active(false),
	m_angular_damping_active(false),
	m_jumping(false)
{
	if (m_bitLocalFlag.ServoControl) {
		// in servo motion, the force is local if the target velocity is local
		m_bitLocalFlag.Force = m_bitLocalFlag.LinearVelocity;

		m_pid = m_torque;
	}
	if (m_bitLocalFlag.CharacterMotion) {
		KX_GameObject *parent = static_cast<KX_GameObject *>(GetParent());
		PHY_ICharacter *character = parent->GetScene()->GetPhysicsEnvironment()->GetCharacterController(parent);

		if (!character) {
			CM_LogicBrickWarning(this, "character motion enabled on non-character object, falling back to simple motion.");
			m_bitLocalFlag.CharacterMotion = false;
		}
	}
	if (m_reference) {
		m_reference->RegisterActuator(this);
	}
	UpdateFuzzyFlags();
}

KX_ObjectActuator::~KX_ObjectActuator()
{
	if (m_reference) {
		m_reference->UnregisterActuator(this);
	}
}

void KX_ObjectActuator::UpdateFuzzyFlags()
{
	m_bitLocalFlag.ZeroForce = mt::FuzzyZero(m_force);
	m_bitLocalFlag.ZeroTorque = mt::FuzzyZero(m_torque);
	m_bitLocalFlag.ZeroDLoc = mt::FuzzyZero(m_dloc);
	m_bitLocalFlag.ZeroDRot = mt::FuzzyZero(m_drot);
	m_bitLocalFlag.ZeroLinearVelocity = mt::FuzzyZero(m_linear_velocity);
	m_linear_length2 = (m_bitLocalFlag.ZeroLinearVelocity) ? 0.0f : m_linear_velocity.LengthSquared();
	m_bitLocalFlag.ZeroAngularVelocity = mt::FuzzyZero(m_angular_velocity);
	m_angular_length2 = (m_bitLocalFlag.ZeroAngularVelocity) ? 0.0f : m_angular_velocity.LengthSquared();
}

bool KX_ObjectActuator::Update()
{
	bool bNegativeEvent = IsNegativeEvent();
	RemoveAllEvents();

	KX_GameObject *parent = static_cast<KX_GameObject *>(GetParent());
	PHY_ICharacter *character = parent->GetScene()->GetPhysicsEnvironment()->GetCharacterController(parent);

	if (bNegativeEvent) {
		// Explicitly stop the movement if we're using character motion
		if (m_bitLocalFlag.CharacterMotion) {
			character->SetWalkDirection(mt::zero3);
		}

		m_linear_damping_active = false;
		m_angular_damping_active = false;
		m_error_accumulator = mt::zero3;
		m_previous_error = mt::zero3;
		m_jumping = false;
		return false;
	}
	else if (parent) {
		if (m_bitLocalFlag.ServoControl) {
			// In this mode, we try to reach a target speed using force
			// As we don't know the friction, we must implement a generic
			// servo control to achieve the speed in a configurable
			// v = current velocity
			// V = target velocity
			// e = V-v = speed error
			// dt = time interval since previous update
			// I = sum(e(t)*dt)
			// dv = e(t) - e(t-1)
			// KP, KD, KI : coefficient
			// F = KP*e+KI*I+KD*dv
			float mass = parent->GetMass();
			if (mt::FuzzyZero(mass)) {
				return false;
			}

			mt::vec3 v;
			if (m_bitLocalFlag.ServoControlAngular) {
				v = parent->GetAngularVelocity(m_bitLocalFlag.AngularVelocity);
			}
			else {
				v = parent->GetLinearVelocity(m_bitLocalFlag.LinearVelocity);
			}

			if (m_reference) {
				if (m_bitLocalFlag.ServoControlAngular) {
					const mt::vec3 vel = m_reference->GetAngularVelocity(m_bitLocalFlag.AngularVelocity);
					v -= vel;
				}
				else {
					const mt::vec3& mypos = parent->NodeGetWorldPosition();
					const mt::vec3& refpos = m_reference->NodeGetWorldPosition();
					const mt::vec3 relpos = (mypos - refpos);
					mt::vec3 vel = m_reference->GetVelocity(relpos);
					if (m_bitLocalFlag.LinearVelocity) {
						// must convert in local space
						vel = parent->NodeGetWorldOrientation().Transpose() * vel;
					}
					v -= vel;
				}
			}

			mt::vec3 e;
			if (m_bitLocalFlag.ServoControlAngular) {
				e = m_angular_velocity - v;
			}
			else {
				e = m_linear_velocity - v;
			}

			mt::vec3 dv = e - m_previous_error;
			mt::vec3 I = m_error_accumulator + e;

			mt::vec3& f = (m_bitLocalFlag.ServoControlAngular) ? m_torque : m_force;
			f = m_pid.x * e + m_pid.y * I + m_pid.z * dv;

			/* Make sure velocity is correct depending on how body react to force/torque.
			 * See btRigidBody::integrateVelocities */
			if (m_bitLocalFlag.ServoControlAngular) {
				f *= parent->GetLocalInertia();
			}
			else {
				f *= mass;
			}

			const bool limits[3] = {m_bitLocalFlag.Torque, m_bitLocalFlag.DLoc, m_bitLocalFlag.DRot};

			for (unsigned short i = 0; i <  3; ++i) {
				if (!limits[i]) {
					continue;
				}

				if (f[i] > m_dloc[i]) {
					f[i] = m_dloc[i];
					I[i] = m_error_accumulator[i];
				}
				else if (f[i] < m_drot[i]) {
					f[i] = m_drot[i];
					I[i] = m_error_accumulator[i];
				}
			}

			m_previous_error = e;
			m_error_accumulator = I;

			if (m_bitLocalFlag.ServoControlAngular) {
				parent->ApplyTorque(f, m_bitLocalFlag.AngularVelocity);
			}
			else {
				parent->ApplyForce(f, m_bitLocalFlag.LinearVelocity);
			}
		}
		else if (m_bitLocalFlag.CharacterMotion) {
			mt::vec3 dir = m_dloc;

			if (m_bitLocalFlag.DLoc) {
				mt::mat3 basis = parent->GetPhysicsController()->GetOrientation();
				dir = basis * dir;
			}

			if (m_bitLocalFlag.AddOrSetCharLoc) {
				mt::vec3 old_dir = character->GetWalkDirection();

				if (!mt::FuzzyZero(old_dir)) {
					float mag = old_dir.Length();

					dir = dir + old_dir;
					if (!mt::FuzzyZero(dir)) {
						dir = dir.Normalized() * mag;
					}
				}
			}

			// We always want to set the walk direction since a walk direction of (0, 0, 0) should stop the character
			character->SetWalkDirection(dir / parent->GetScene()->GetPhysicsEnvironment()->GetNumTimeSubSteps());

			if (!m_bitLocalFlag.ZeroDRot) {
				parent->ApplyRotation(m_drot, (m_bitLocalFlag.DRot) != 0);
			}

			if (m_bitLocalFlag.CharacterJump) {
				if (!m_jumping) {
					character->Jump();
					m_jumping = true;
				}
				else if (character->OnGround()) {
					m_jumping = false;
				}
			}
		}
		else {
			if (!m_bitLocalFlag.ZeroForce) {
				parent->ApplyForce(m_force, (m_bitLocalFlag.Force) != 0);
			}
			if (!m_bitLocalFlag.ZeroTorque) {
				parent->ApplyTorque(m_torque, (m_bitLocalFlag.Torque) != 0);
			}
			if (!m_bitLocalFlag.ZeroDLoc) {
				parent->ApplyMovement(m_dloc, (m_bitLocalFlag.DLoc) != 0);
			}
			if (!m_bitLocalFlag.ZeroDRot) {
				parent->ApplyRotation(m_drot, (m_bitLocalFlag.DRot) != 0);
			}
			if (!m_bitLocalFlag.ZeroLinearVelocity) {
				if (m_bitLocalFlag.AddOrSetLinV) {
					parent->AddLinearVelocity(m_linear_velocity, (m_bitLocalFlag.LinearVelocity) != 0);
				}
				else {
					if (m_damping > 0) {
						mt::vec3 linV;
						if (!m_linear_damping_active) {
							// delta and the start speed (depends on the existing speed in that direction)
							linV = parent->GetLinearVelocity(m_bitLocalFlag.LinearVelocity);
							// keep only the projection along the desired direction
							m_current_linear_factor = mt::dot(linV, m_linear_velocity) / m_linear_length2;
							m_linear_damping_active = true;
						}
						if (m_current_linear_factor < 1.0f) {
							m_current_linear_factor += 1.0f / m_damping;
						}
						if (m_current_linear_factor > 1.0f) {
							m_current_linear_factor = 1.0f;
						}
						linV = m_current_linear_factor * m_linear_velocity;
						parent->SetLinearVelocity(linV, (m_bitLocalFlag.LinearVelocity) != 0);
					}
					else {
						parent->SetLinearVelocity(m_linear_velocity, (m_bitLocalFlag.LinearVelocity) != 0);
					}
				}
			}
			if (!m_bitLocalFlag.ZeroAngularVelocity) {
				if (m_damping > 0) {
					mt::vec3 angV;
					if (!m_angular_damping_active) {
						// delta and the start speed (depends on the existing speed in that direction)
						angV = parent->GetAngularVelocity(m_bitLocalFlag.AngularVelocity);
						// keep only the projection along the desired direction
						m_current_angular_factor = mt::dot(angV, m_angular_velocity) / m_angular_length2;
						m_angular_damping_active = true;
					}
					if (m_current_angular_factor < 1.0) {
						m_current_angular_factor += 1.0 / m_damping;
					}
					if (m_current_angular_factor > 1.0) {
						m_current_angular_factor = 1.0;
					}
					angV = m_current_angular_factor * m_angular_velocity;
					parent->SetAngularVelocity(angV, (m_bitLocalFlag.AngularVelocity) != 0);
				}
				else {
					parent->SetAngularVelocity(m_angular_velocity, (m_bitLocalFlag.AngularVelocity) != 0);
				}
			}
		}

	}
	return true;
}

EXP_Value *KX_ObjectActuator::GetReplica()
{
	KX_ObjectActuator *replica = new KX_ObjectActuator(*this);
	replica->ProcessReplica();

	return replica;
}

void KX_ObjectActuator::ProcessReplica()
{
	SCA_IActuator::ProcessReplica();
	if (m_reference) {
		m_reference->RegisterActuator(this);
	}
}

bool KX_ObjectActuator::UnlinkObject(SCA_IObject *clientobj)
{
	if (clientobj == m_reference) {
		// this object is being deleted, we cannot continue to use it as reference.
		m_reference = nullptr;
		return true;
	}
	return false;
}

void KX_ObjectActuator::Relink(std::map<SCA_IObject *, SCA_IObject *>& obj_map)
{
	KX_GameObject *obj = static_cast<KX_GameObject *>(obj_map[m_reference]);
	if (obj) {
		if (m_reference) {
			m_reference->UnregisterActuator(this);
		}
		m_reference = obj;
		m_reference->RegisterActuator(this);
	}
}

#ifdef WITH_PYTHON

/* ------------------------------------------------------------------------- */
/* Python functions                                                          */
/* ------------------------------------------------------------------------- */

/* Integration hooks ------------------------------------------------------- */
PyTypeObject KX_ObjectActuator::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"KX_ObjectActuator",
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

PyMethodDef KX_ObjectActuator::Methods[] = {
	{nullptr, nullptr} //Sentinel
};

PyAttributeDef KX_ObjectActuator::Attributes[] = {
	EXP_PYATTRIBUTE_VECTOR_RW_CHECK("force", -1000, 1000, false, KX_ObjectActuator, m_force, 3, PyUpdateFuzzyFlags),
	EXP_PYATTRIBUTE_BOOL_RW("useLocalForce", KX_ObjectActuator, m_bitLocalFlag.Force),
	EXP_PYATTRIBUTE_VECTOR_RW_CHECK("torque", -1000, 1000, false, KX_ObjectActuator, m_torque, 3, PyUpdateFuzzyFlags),
	EXP_PYATTRIBUTE_BOOL_RW("useLocalTorque", KX_ObjectActuator, m_bitLocalFlag.Torque),
	EXP_PYATTRIBUTE_VECTOR_RW_CHECK("dLoc", -1000, 1000, false, KX_ObjectActuator, m_dloc, 3, PyUpdateFuzzyFlags),
	EXP_PYATTRIBUTE_BOOL_RW("useLocalDLoc", KX_ObjectActuator, m_bitLocalFlag.DLoc),
	EXP_PYATTRIBUTE_VECTOR_RW_CHECK("dRot", -1000, 1000, false, KX_ObjectActuator, m_drot, 3, PyUpdateFuzzyFlags),
	EXP_PYATTRIBUTE_BOOL_RW("useLocalDRot", KX_ObjectActuator, m_bitLocalFlag.DRot),
#ifdef USE_MATHUTILS
	EXP_PYATTRIBUTE_RW_FUNCTION("linV", KX_ObjectActuator, pyattr_get_linV, pyattr_set_linV),
	EXP_PYATTRIBUTE_RW_FUNCTION("angV", KX_ObjectActuator, pyattr_get_angV, pyattr_set_angV),
#else
	EXP_PYATTRIBUTE_VECTOR_RW_CHECK("linV", -1000, 1000, false, KX_ObjectActuator, m_linear_velocity, 3, PyUpdateFuzzyFlags),
	EXP_PYATTRIBUTE_VECTOR_RW_CHECK("angV", -1000, 1000, false, KX_ObjectActuator, m_angular_velocity, 3, PyUpdateFuzzyFlags),
#endif
	EXP_PYATTRIBUTE_BOOL_RW("useLocalLinV", KX_ObjectActuator, m_bitLocalFlag.LinearVelocity),
	EXP_PYATTRIBUTE_BOOL_RW("useLocalAngV", KX_ObjectActuator, m_bitLocalFlag.AngularVelocity),
	EXP_PYATTRIBUTE_SHORT_RW("damping", 0, 1000, false, KX_ObjectActuator, m_damping),
	EXP_PYATTRIBUTE_RW_FUNCTION("forceLimitX", KX_ObjectActuator, pyattr_get_forceLimitX, pyattr_set_forceLimitX),
	EXP_PYATTRIBUTE_RW_FUNCTION("forceLimitY", KX_ObjectActuator, pyattr_get_forceLimitY, pyattr_set_forceLimitY),
	EXP_PYATTRIBUTE_RW_FUNCTION("forceLimitZ", KX_ObjectActuator, pyattr_get_forceLimitZ, pyattr_set_forceLimitZ),
	EXP_PYATTRIBUTE_VECTOR_RW_CHECK("pid", -100, 200, true, KX_ObjectActuator, m_pid, 3, PyCheckPid),
	EXP_PYATTRIBUTE_RW_FUNCTION("reference", KX_ObjectActuator, pyattr_get_reference, pyattr_set_reference),
	EXP_PYATTRIBUTE_NULL //Sentinel
};

/* Attribute get/set functions */

#ifdef USE_MATHUTILS

/* These require an SGNode */
#define MATHUTILS_VEC_CB_LINV 1
#define MATHUTILS_VEC_CB_ANGV 2

static unsigned char mathutils_kxobactu_vector_cb_index = -1; /* index for our callbacks */

static int mathutils_obactu_generic_check(BaseMathObject *bmo)
{
	KX_ObjectActuator *self = static_cast<KX_ObjectActuator *>EXP_PROXY_REF(bmo->cb_user);
	if (self == nullptr) {
		return -1;
	}

	return 0;
}

static int mathutils_obactu_vector_get(BaseMathObject *bmo, int subtype)
{
	KX_ObjectActuator *self = static_cast<KX_ObjectActuator *>EXP_PROXY_REF(bmo->cb_user);
	if (self == nullptr) {
		return -1;
	}

	switch (subtype) {
		case MATHUTILS_VEC_CB_LINV:
		{
			self->m_linear_velocity.Pack(bmo->data);
			break;
		}
		case MATHUTILS_VEC_CB_ANGV:
		{
			self->m_angular_velocity.Pack(bmo->data);
			break;
		}
	}

	return 0;
}

static int mathutils_obactu_vector_set(BaseMathObject *bmo, int subtype)
{
	KX_ObjectActuator *self = static_cast<KX_ObjectActuator *>EXP_PROXY_REF(bmo->cb_user);
	if (self == nullptr) {
		return -1;
	}

	switch (subtype) {
		case MATHUTILS_VEC_CB_LINV:
		{
			self->m_linear_velocity = mt::vec3(bmo->data);
			break;
		}
		case MATHUTILS_VEC_CB_ANGV:
		{
			self->m_angular_velocity = mt::vec3(bmo->data);
			break;
		}
	}

	return 0;
}

static int mathutils_obactu_vector_get_index(BaseMathObject *bmo, int subtype, int index)
{
	/* lazy, avoid repeteing the case statement */
	if (mathutils_obactu_vector_get(bmo, subtype) == -1) {
		return -1;
	}
	return 0;
}

static int mathutils_obactu_vector_set_index(BaseMathObject *bmo, int subtype, int index)
{
	float f = bmo->data[index];

	/* lazy, avoid repeteing the case statement */
	if (mathutils_obactu_vector_get(bmo, subtype) == -1) {
		return -1;
	}

	bmo->data[index] = f;
	return mathutils_obactu_vector_set(bmo, subtype);
}

static Mathutils_Callback mathutils_obactu_vector_cb = {
	mathutils_obactu_generic_check,
	mathutils_obactu_vector_get,
	mathutils_obactu_vector_set,
	mathutils_obactu_vector_get_index,
	mathutils_obactu_vector_set_index
};

PyObject *KX_ObjectActuator::pyattr_get_linV(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	return Vector_CreatePyObject_cb(
		EXP_PROXY_FROM_REF_BORROW(self_v), 3,
		mathutils_kxobactu_vector_cb_index, MATHUTILS_VEC_CB_LINV);
}

int KX_ObjectActuator::pyattr_set_linV(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_ObjectActuator *self = static_cast<KX_ObjectActuator *>(self_v);
	if (!PyVecTo(value, self->m_linear_velocity)) {
		return PY_SET_ATTR_FAIL;
	}

	self->UpdateFuzzyFlags();

	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_ObjectActuator::pyattr_get_angV(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	return Vector_CreatePyObject_cb(
		EXP_PROXY_FROM_REF_BORROW(self_v), 3,
		mathutils_kxobactu_vector_cb_index, MATHUTILS_VEC_CB_ANGV);
}

int KX_ObjectActuator::pyattr_set_angV(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_ObjectActuator *self = static_cast<KX_ObjectActuator *>(self_v);
	if (!PyVecTo(value, self->m_angular_velocity)) {
		return PY_SET_ATTR_FAIL;
	}

	self->UpdateFuzzyFlags();

	return PY_SET_ATTR_SUCCESS;
}

void KX_ObjectActuator_Mathutils_Callback_Init(void)
{
	// register mathutils callbacks, ok to run more than once.
	mathutils_kxobactu_vector_cb_index = Mathutils_RegisterCallback(&mathutils_obactu_vector_cb);
}

#endif // USE_MATHUTILS

PyObject *KX_ObjectActuator::pyattr_get_forceLimitX(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_ObjectActuator *self = reinterpret_cast<KX_ObjectActuator *>(self_v);
	PyObject *retVal = PyList_New(3);

	PyList_SET_ITEM(retVal, 0, PyFloat_FromDouble(self->m_drot[0]));
	PyList_SET_ITEM(retVal, 1, PyFloat_FromDouble(self->m_dloc[0]));
	PyList_SET_ITEM(retVal, 2, PyBool_FromLong(self->m_bitLocalFlag.Torque));

	return retVal;
}

int KX_ObjectActuator::pyattr_set_forceLimitX(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_ObjectActuator *self = reinterpret_cast<KX_ObjectActuator *>(self_v);

	PyObject *seq = PySequence_Fast(value, "");
	if (seq && PySequence_Fast_GET_SIZE(seq) == 3) {
		self->m_drot[0] = PyFloat_AsDouble(PySequence_Fast_GET_ITEM(value, 0));
		self->m_dloc[0] = PyFloat_AsDouble(PySequence_Fast_GET_ITEM(value, 1));
		self->m_bitLocalFlag.Torque = (PyLong_AsLong(PySequence_Fast_GET_ITEM(value, 2)) != 0);

		if (!PyErr_Occurred()) {
			Py_DECREF(seq);
			return PY_SET_ATTR_SUCCESS;
		}
	}

	Py_XDECREF(seq);

	PyErr_SetString(PyExc_ValueError, "expected a sequence of 2 floats and a bool");
	return PY_SET_ATTR_FAIL;
}

PyObject *KX_ObjectActuator::pyattr_get_forceLimitY(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_ObjectActuator *self = reinterpret_cast<KX_ObjectActuator *>(self_v);
	PyObject *retVal = PyList_New(3);

	PyList_SET_ITEM(retVal, 0, PyFloat_FromDouble(self->m_drot[1]));
	PyList_SET_ITEM(retVal, 1, PyFloat_FromDouble(self->m_dloc[1]));
	PyList_SET_ITEM(retVal, 2, PyBool_FromLong(self->m_bitLocalFlag.DLoc));

	return retVal;
}

int KX_ObjectActuator::pyattr_set_forceLimitY(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_ObjectActuator *self = reinterpret_cast<KX_ObjectActuator *>(self_v);

	PyObject *seq = PySequence_Fast(value, "");
	if (seq && PySequence_Fast_GET_SIZE(seq) == 3) {
		self->m_drot[1] = PyFloat_AsDouble(PySequence_Fast_GET_ITEM(value, 0));
		self->m_dloc[1] = PyFloat_AsDouble(PySequence_Fast_GET_ITEM(value, 1));
		self->m_bitLocalFlag.DLoc = (PyLong_AsLong(PySequence_Fast_GET_ITEM(value, 2)) != 0);

		if (!PyErr_Occurred()) {
			Py_DECREF(seq);
			return PY_SET_ATTR_SUCCESS;
		}
	}

	Py_XDECREF(seq);

	PyErr_SetString(PyExc_ValueError, "expected a sequence of 2 floats and a bool");
	return PY_SET_ATTR_FAIL;
}

PyObject *KX_ObjectActuator::pyattr_get_forceLimitZ(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_ObjectActuator *self = reinterpret_cast<KX_ObjectActuator *>(self_v);
	PyObject *retVal = PyList_New(3);

	PyList_SET_ITEM(retVal, 0, PyFloat_FromDouble(self->m_drot[2]));
	PyList_SET_ITEM(retVal, 1, PyFloat_FromDouble(self->m_dloc[2]));
	PyList_SET_ITEM(retVal, 2, PyBool_FromLong(self->m_bitLocalFlag.DRot));

	return retVal;
}

int KX_ObjectActuator::pyattr_set_forceLimitZ(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_ObjectActuator *self = reinterpret_cast<KX_ObjectActuator *>(self_v);

	PyObject *seq = PySequence_Fast(value, "");
	if (seq && PySequence_Fast_GET_SIZE(seq) == 3) {
		self->m_drot[2] = PyFloat_AsDouble(PySequence_Fast_GET_ITEM(value, 0));
		self->m_dloc[2] = PyFloat_AsDouble(PySequence_Fast_GET_ITEM(value, 1));
		self->m_bitLocalFlag.DRot = (PyLong_AsLong(PySequence_Fast_GET_ITEM(value, 2)) != 0);

		if (!PyErr_Occurred()) {
			Py_DECREF(seq);
			return PY_SET_ATTR_SUCCESS;
		}
	}

	Py_XDECREF(seq);

	PyErr_SetString(PyExc_ValueError, "expected a sequence of 2 floats and a bool");
	return PY_SET_ATTR_FAIL;
}

PyObject *KX_ObjectActuator::pyattr_get_reference(EXP_PyObjectPlus *self, const struct EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_ObjectActuator *actuator = static_cast<KX_ObjectActuator *>(self);
	if (!actuator->m_reference) {
		Py_RETURN_NONE;
	}

	return actuator->m_reference->GetProxy();
}

int KX_ObjectActuator::pyattr_set_reference(EXP_PyObjectPlus *self, const struct EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_ObjectActuator *actuator = static_cast<KX_ObjectActuator *>(self);
	KX_GameObject *refOb;

	if (!ConvertPythonToGameObject(actuator->GetLogicManager(), value, &refOb, true, "actu.reference = value: KX_ObjectActuator")) {
		return PY_SET_ATTR_FAIL;
	}

	if (actuator->m_reference) {
		actuator->m_reference->UnregisterActuator(actuator);
	}

	if (refOb == nullptr) {
		actuator->m_reference = nullptr;
	}
	else {
		actuator->m_reference = refOb;
		actuator->m_reference->RegisterActuator(actuator);
	}

	return PY_SET_ATTR_SUCCESS;
}

// This lets the attribute macros use UpdateFuzzyFlags()
int KX_ObjectActuator::PyUpdateFuzzyFlags(EXP_PyObjectPlus *self, const PyAttributeDef *attrdef)
{
	KX_ObjectActuator *act = reinterpret_cast<KX_ObjectActuator *>(self);
	act->UpdateFuzzyFlags();
	return 0;
}

// This is the keep the PID values in check after they are assigned with Python
int KX_ObjectActuator::PyCheckPid(EXP_PyObjectPlus *self, const PyAttributeDef *attrdef)
{
	KX_ObjectActuator *act = reinterpret_cast<KX_ObjectActuator *>(self);

	// P 0 to 200
	if (act->m_pid[0] < 0) {
		act->m_pid[0] = 0;
	}
	else if (act->m_pid[0] > 200) {
		act->m_pid[0] = 200;
	}

	// I 0 to 3
	if (act->m_pid[1] < 0) {
		act->m_pid[1] = 0;
	}
	else if (act->m_pid[1] > 3) {
		act->m_pid[1] = 3;
	}

	// D -100 to 100
	if (act->m_pid[2] < -100) {
		act->m_pid[2] = -100;
	}
	else if (act->m_pid[2] > 100) {
		act->m_pid[2] = 100;
	}

	return 0;
}

#endif  // WITH_PYTHON
