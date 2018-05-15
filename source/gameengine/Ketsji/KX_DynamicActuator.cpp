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

/** \file gameengine/Ketsji/KX_DynamicActuator.cpp
 *  \ingroup ketsji
 * Adjust dynamics settings for this object
 */

/* Previously existed as:
 * \source\gameengine\GameLogic\SCA_DynamicActuator.cpp
 * Please look here for revision history. */

#include "KX_DynamicActuator.h"
#include "PHY_IPhysicsController.h"

#ifdef WITH_PYTHON

/* ------------------------------------------------------------------------- */
/* Python functions                                                          */
/* ------------------------------------------------------------------------- */

/* Integration hooks ------------------------------------------------------- */

PyTypeObject KX_DynamicActuator::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"KX_DynamicActuator",
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

PyMethodDef KX_DynamicActuator::Methods[] = {
	{nullptr, nullptr} //Sentinel
};

PyAttributeDef KX_DynamicActuator::Attributes[] = {
	EXP_PYATTRIBUTE_SHORT_RW("mode", 0, 4, false, KX_DynamicActuator, m_dyn_operation),
	EXP_PYATTRIBUTE_FLOAT_RW("mass", 0.0f, FLT_MAX, KX_DynamicActuator, m_setmass),
	EXP_PYATTRIBUTE_NULL    //Sentinel
};

#endif // WITH_PYTHON

/* ------------------------------------------------------------------------- */
/* Native functions                                                          */
/* ------------------------------------------------------------------------- */

KX_DynamicActuator::KX_DynamicActuator(SCA_IObject *gameobj,
                                       short dyn_operation,
                                       float setmass) :

	SCA_IActuator(gameobj, KX_ACT_DYNAMIC),
	m_dyn_operation(dyn_operation),
	m_setmass(setmass)
{
} /* End of constructor */


KX_DynamicActuator::~KX_DynamicActuator()
{
	// there's nothing to be done here, really....
} /* end of destructor */



bool KX_DynamicActuator::Update()
{
	// bool result = false;	/*unused*/
	KX_GameObject *obj = (KX_GameObject *)GetParent();
	bool bNegativeEvent = IsNegativeEvent();
	PHY_IPhysicsController *controller;
	RemoveAllEvents();

	if (bNegativeEvent) {
		return false; // do nothing on negative events

	}
	if (!obj) {
		return false; // object not accessible, shouldnt happen
	}
	controller = obj->GetPhysicsController();
	if (!controller) {
		return false;   // no physic object

	}
	switch (m_dyn_operation) {
		case KX_DYN_RESTORE_DYNAMICS:
		{
			// Child objects must be static, so we block changing to dynamic
			if (!obj->GetParent()) {
				controller->RestoreDynamics();
			}
			break;
		}
		case KX_DYN_DISABLE_DYNAMICS:
		{
			controller->SuspendDynamics();
			break;
		}
		case KX_DYN_ENABLE_RIGID_BODY:
		{
			controller->SetRigidBody(true);
			break;
		}
		case KX_DYN_DISABLE_RIGID_BODY:
		{
			controller->SetRigidBody(false);
			break;
		}
		case KX_DYN_SET_MASS:
		{
			controller->SetMass(m_setmass);
			break;
		}
		case KX_DYN_RESTORE_PHYSICS:
		{
			controller->RestorePhysics();
			break;
		}
		case KX_DYN_DISABLE_PHYSICS:
		{
			controller->SuspendPhysics(false);
			break;
		}
	}

	return false;
}



EXP_Value *KX_DynamicActuator::GetReplica()
{
	KX_DynamicActuator *replica =
		new KX_DynamicActuator(*this);

	if (replica == nullptr) {
		return nullptr;
	}

	replica->ProcessReplica();
	return replica;
};


/* eof */
