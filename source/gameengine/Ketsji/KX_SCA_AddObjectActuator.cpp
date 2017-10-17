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

/** \file gameengine/Ketsji/KX_SCA_AddObjectActuator.cpp
 *  \ingroup ketsji
 *
 * Add an object when this actuator is triggered
 */

/* Previously existed as:
 * \source\gameengine\GameLogic\SCA_AddObjectActuator.cpp
 * Please look here for revision history. */

#include "KX_SCA_AddObjectActuator.h"
#include "KX_Scene.h"
#include "KX_GameObject.h"

/* ------------------------------------------------------------------------- */
/* Native functions                                                          */
/* ------------------------------------------------------------------------- */

KX_SCA_AddObjectActuator::KX_SCA_AddObjectActuator(KX_GameObject *gameobj,
												   KX_GameObject *original,
												   float time,
												   KX_Scene *scene,
												   const float *linvel,
												   bool linv_local,
												   const float *angvel,
												   bool angv_local)
	: 
	SCA_IActuator(gameobj, KX_ACT_ADD_OBJECT),
	m_OriginalObject(original),
	m_scene(scene),
	
	m_localLinvFlag(linv_local),
	m_localAngvFlag(angv_local)
{
	m_linear_velocity[0] = linvel[0];
	m_linear_velocity[1] = linvel[1];
	m_linear_velocity[2] = linvel[2];
	m_angular_velocity[0] = angvel[0];
	m_angular_velocity[1] = angvel[1];
	m_angular_velocity[2] = angvel[2];

	if (m_OriginalObject)
		m_OriginalObject->RegisterActuator(this);

	m_lastCreatedObject = nullptr;
	m_timeProp = time;
} 



KX_SCA_AddObjectActuator::~KX_SCA_AddObjectActuator()
{ 
	if (m_OriginalObject)
		m_OriginalObject->UnregisterActuator(this);
	if (m_lastCreatedObject)
		m_lastCreatedObject->UnregisterActuator(this);
} 



bool KX_SCA_AddObjectActuator::Update()
{
	//bool result = false;	/*unused*/
	bool bNegativeEvent = IsNegativeEvent();
	RemoveAllEvents();
	
	if (bNegativeEvent) return false; // do nothing on negative events

	InstantAddObject();

	
	return false;
}




KX_GameObject *KX_SCA_AddObjectActuator::GetLastCreatedObject() const 
{
	return m_lastCreatedObject;
}



EXP_Value* KX_SCA_AddObjectActuator::GetReplica() 
{
	KX_SCA_AddObjectActuator* replica = new KX_SCA_AddObjectActuator(*this);

	if (replica == nullptr)
		return nullptr;

	// this will copy properties and so on...
	replica->ProcessReplica();

	return replica;
}

void KX_SCA_AddObjectActuator::ProcessReplica()
{
	if (m_OriginalObject)
		m_OriginalObject->RegisterActuator(this);
	m_lastCreatedObject=nullptr;
	SCA_IActuator::ProcessReplica();
}

void KX_SCA_AddObjectActuator::Replace_IScene(SCA_IScene *val)
{
	m_scene = static_cast<KX_Scene *>(val);
}

bool KX_SCA_AddObjectActuator::UnlinkObject(SCA_IObject* clientobj)
{
	if (clientobj == m_OriginalObject)
	{
		// this object is being deleted, we cannot continue to track it.
		m_OriginalObject = nullptr;
		return true;
	}
	if (clientobj == m_lastCreatedObject)
	{
		// this object is being deleted, we cannot continue to track it.
		m_lastCreatedObject = nullptr;
		return true;
	}
	return false;
}

void KX_SCA_AddObjectActuator::Relink(std::map<SCA_IObject *, SCA_IObject *>& obj_map)
{
	SCA_IObject *obj = obj_map[m_OriginalObject];
	if (obj) {
		if (m_OriginalObject)
			m_OriginalObject->UnregisterActuator(this);
		m_OriginalObject = static_cast<KX_GameObject *>(obj);
		m_OriginalObject->RegisterActuator(this);
	}
}

#ifdef WITH_PYTHON

/* ------------------------------------------------------------------------- */
/* Python functions                                                          */
/* ------------------------------------------------------------------------- */

/* Integration hooks ------------------------------------------------------- */
PyTypeObject KX_SCA_AddObjectActuator::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"KX_SCA_AddObjectActuator",
	sizeof(EXP_PyObjectPlus_Proxy),
	0,
	py_base_dealloc,
	0,
	0,
	0,
	0,
	py_base_repr,
	0,0,0,0,0,0,0,0,0,
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	0,0,0,0,0,0,0,
	Methods,
	0,
	0,
	&SCA_IActuator::Type,
	0,0,0,0,0,0,
	py_base_new
};

PyMethodDef KX_SCA_AddObjectActuator::Methods[] = {
	{"instantAddObject", (PyCFunction) KX_SCA_AddObjectActuator::sPyInstantAddObject, METH_NOARGS, nullptr},
	{nullptr,nullptr} //Sentinel
};

PyAttributeDef KX_SCA_AddObjectActuator::Attributes[] = {
	EXP_PYATTRIBUTE_RW_FUNCTION("object",KX_SCA_AddObjectActuator,pyattr_get_object,pyattr_set_object),
	EXP_PYATTRIBUTE_RO_FUNCTION("objectLastCreated",KX_SCA_AddObjectActuator,pyattr_get_objectLastCreated),
	EXP_PYATTRIBUTE_FLOAT_RW("time", 0.0f, FLT_MAX, KX_SCA_AddObjectActuator, m_timeProp),
	EXP_PYATTRIBUTE_FLOAT_ARRAY_RW("linearVelocity",-FLT_MAX,FLT_MAX,KX_SCA_AddObjectActuator,m_linear_velocity,3),
	EXP_PYATTRIBUTE_FLOAT_ARRAY_RW("angularVelocity",-FLT_MAX,FLT_MAX,KX_SCA_AddObjectActuator,m_angular_velocity,3),
	EXP_PYATTRIBUTE_NULL	//Sentinel
};

PyObject *KX_SCA_AddObjectActuator::pyattr_get_object(EXP_PyObjectPlus *self, const struct EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_SCA_AddObjectActuator* actuator = static_cast<KX_SCA_AddObjectActuator*>(self);
	if (!actuator->m_OriginalObject)
		Py_RETURN_NONE;
	else
		return actuator->m_OriginalObject->GetProxy();
}

int KX_SCA_AddObjectActuator::pyattr_set_object(EXP_PyObjectPlus *self, const struct EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_SCA_AddObjectActuator* actuator = static_cast<KX_SCA_AddObjectActuator*>(self);
	KX_GameObject *gameobj;
		
	if (!ConvertPythonToGameObject(actuator->GetLogicManager(), value, &gameobj, true, "actuator.object = value: KX_SCA_AddObjectActuator"))
		return PY_SET_ATTR_FAIL; // ConvertPythonToGameObject sets the error
		
	if (actuator->m_OriginalObject != nullptr)
		actuator->m_OriginalObject->UnregisterActuator(actuator);

	actuator->m_OriginalObject = gameobj;
		
	if (actuator->m_OriginalObject)
		actuator->m_OriginalObject->RegisterActuator(actuator);
		
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_SCA_AddObjectActuator::pyattr_get_objectLastCreated(EXP_PyObjectPlus *self, const struct EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_SCA_AddObjectActuator* actuator = static_cast<KX_SCA_AddObjectActuator*>(self);
	if (!actuator->m_lastCreatedObject)
		Py_RETURN_NONE;
	else
		return actuator->m_lastCreatedObject->GetProxy();
}

PyObject *KX_SCA_AddObjectActuator::PyInstantAddObject()
{
	InstantAddObject();

	Py_RETURN_NONE;
}

#endif // WITH_PYTHON

void	KX_SCA_AddObjectActuator::InstantAddObject()
{
	if (m_OriginalObject)
	{
		// Add an identical object, with properties inherited from the original object
		// Now it needs to be added to the current scene.
		KX_GameObject *replica = m_scene->AddReplicaObject(m_OriginalObject, static_cast<KX_GameObject *>(GetParent()), m_timeProp);
		replica->setLinearVelocity(MT_Vector3(m_linear_velocity), m_localLinvFlag);
		replica->setAngularVelocity(MT_Vector3(m_angular_velocity),m_localAngvFlag);

		// keep a copy of the last object, to allow python scripters to change it
		if (m_lastCreatedObject)
		{
			//Let's not keep a reference to the object: it's bad, if the object is deleted
			//this will force to keep a "zombie" in the game for no good reason.
			//m_scene->DelayedReleaseObject(m_lastCreatedObject);
			//m_lastCreatedObject->Release();

			//Instead we use the registration mechanism
			m_lastCreatedObject->UnregisterActuator(this);
			m_lastCreatedObject = nullptr;
		}
		
		m_lastCreatedObject = replica;
		// no reference
		//m_lastCreatedObject->AddRef();
		// but registration
		m_lastCreatedObject->RegisterActuator(this);
		// finished using replica? then release it
		replica->Release();
	}
}
