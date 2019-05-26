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

/** \file KX_ObjectActuator.h
 *  \ingroup ketsji
 *  \brief Do translation/rotation actions
 */

#ifndef __KX_OBJECTACTUATOR_H__
#define __KX_OBJECTACTUATOR_H__

#include "SCA_IActuator.h"
#include "mathfu.h"

#ifdef USE_MATHUTILS
void KX_ObjectActuator_Mathutils_Callback_Init(void);
#endif

class KX_GameObject;

struct KX_LocalFlags {
	KX_LocalFlags() 
		:Force(false),
		Torque(false),
		DRot(false),
		DLoc(false),
		LinearVelocity(false),
		AngularVelocity(false),
		AddOrSetLinV(false),
		AddOrSetCharLoc(false),
		ServoControl(false),
		CharacterMotion(false),
		CharacterJump(false),
		ZeroForce(false),
		ZeroTorque(false),
		ZeroDRot(false),
		ZeroDLoc(false),
		ZeroLinearVelocity(false),
		ZeroAngularVelocity(false)
	{
	}

	bool Force;
	bool Torque;
	bool DRot;
	bool DLoc;
	bool LinearVelocity;
	bool AngularVelocity;
	bool AddOrSetLinV;
	bool AddOrSetCharLoc;
	bool ServoControl;
	bool CharacterMotion;
	bool CharacterJump;
	bool ZeroForce;
	bool ZeroTorque;
	bool ZeroDRot;
	bool ZeroDLoc;
	bool ZeroLinearVelocity;
	bool ZeroAngularVelocity;
	bool ServoControlAngular;
};

class KX_ObjectActuator : public SCA_IActuator, public mt::SimdClassAllocator
{
	Py_Header

	mt::vec3 m_force;
	mt::vec3 m_torque;
	mt::vec3 m_dloc;
	mt::vec3 m_drot;
	mt::vec3 m_linear_velocity;
	mt::vec3 m_angular_velocity;
	mt::vec3 m_pid;
	float m_linear_length2;
	float m_angular_length2;
	// used in damping
	float m_current_linear_factor;
	float m_current_angular_factor;
	short m_damping;
	// used in servo control
	mt::vec3 m_previous_error;
	mt::vec3 m_error_accumulator;
	KX_LocalFlags m_bitLocalFlag;
	KX_GameObject *m_reference;

	bool m_linear_damping_active;
	bool m_angular_damping_active;
	bool m_jumping;

public:
	KX_ObjectActuator(SCA_IObject *gameobj,
	                  KX_GameObject *refobj,
	                  const mt::vec3& force,
	                  const mt::vec3& torque,
	                  const mt::vec3& dloc,
	                  const mt::vec3& drot,
	                  const mt::vec3& linV,
	                  const mt::vec3& angV,
	                  const short damping,
	                  const KX_LocalFlags& flag);
	virtual ~KX_ObjectActuator();

	virtual EXP_Value *GetReplica();
	virtual void ProcessReplica();
	virtual bool UnlinkObject(SCA_IObject *clientobj);
	virtual void Relink(std::map<SCA_IObject *, SCA_IObject *>& obj_map);

	void UpdateFuzzyFlags();

	virtual bool Update();

#ifdef WITH_PYTHON

	static PyObject *pyattr_get_forceLimitX(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_forceLimitX(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_forceLimitY(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_forceLimitY(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_forceLimitZ(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_forceLimitZ(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_reference(EXP_PyObjectPlus *self, const struct EXP_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_reference(EXP_PyObjectPlus *self, const struct EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);

#ifdef USE_MATHUTILS
	static PyObject *pyattr_get_linV(EXP_PyObjectPlus *self, const struct EXP_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_linV(EXP_PyObjectPlus *self, const struct EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_angV(EXP_PyObjectPlus *self, const struct EXP_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_angV(EXP_PyObjectPlus *self, const struct EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
#endif

	/// This lets the attribute macros use UpdateFuzzyFlags().
	static int PyUpdateFuzzyFlags(EXP_PyObjectPlus *self, const PyAttributeDef *attrdef);
	/// This is the keep the PID values in check after they are assigned with Python.
	static int PyCheckPid(EXP_PyObjectPlus *self, const PyAttributeDef *attrdef);

#endif  // WITH_PYTHON

};

#endif  // __KX_OBJECTACTUATOR_H__
