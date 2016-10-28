/*
 * Senses touch and collision events
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

/** \file gameengine/Ketsji/KX_CollisionSensor.cpp
 *  \ingroup ketsji
 */


#include "KX_CollisionSensor.h"
#include "SCA_EventManager.h"
#include "SCA_LogicManager.h"
#include "KX_GameObject.h"
#include "KX_CollisionEventManager.h"

#include "PHY_IPhysicsController.h"

#include "RAS_MeshObject.h"

#include <iostream>
#include "BLI_utildefines.h"
#include "PHY_IPhysicsEnvironment.h"

/* ------------------------------------------------------------------------- */
/* Native functions                                                          */
/* ------------------------------------------------------------------------- */

void KX_CollisionSensor::SynchronizeTransform()
{
	// the touch sensor does not require any synchronization: it uses
	// the same physical object which is already synchronized by Blender
}


void KX_CollisionSensor::EndFrame()
{
	m_colliders->ReleaseAndRemoveAll();
	m_hitObject = NULL;
	m_bTriggered = false;
	m_bColliderHash = 0;
}

void KX_CollisionSensor::UnregisterToManager()
{
	// before unregistering the sensor, make sure we release all references
	EndFrame();
	SCA_ISensor::UnregisterToManager();
}

bool KX_CollisionSensor::Evaluate()
{
	bool result = false;
	bool reset = m_reset && m_level;
	m_reset = false;
	if (m_bTriggered != m_bLastTriggered)
	{
		m_bLastTriggered = m_bTriggered;
		if (!m_bTriggered)
			m_hitObject = NULL;
		result = true;
	}
	if (reset)
		// force an event
		result = true;
	
	if (m_bCollisionPulse) { /* pulse on changes to the colliders */
		int count = m_colliders->GetCount();
		
		if (m_bLastCount!=count || m_bColliderHash!=m_bLastColliderHash) {
			m_bLastCount = count;
			m_bLastColliderHash= m_bColliderHash;
			result = true;
		}
	}
	return result;
}

KX_CollisionSensor::KX_CollisionSensor(SCA_EventManager* eventmgr,KX_GameObject* gameobj,bool bFindMaterial,bool bCollisionPulse,const STR_String& touchedpropname)
:SCA_ISensor(gameobj,eventmgr),
m_touchedpropname(touchedpropname),
m_bFindMaterial(bFindMaterial),
m_bCollisionPulse(bCollisionPulse),
m_hitMaterial("")
/*m_sumoObj(sumoObj),*/
{
//	KX_CollisionEventManager* collisionmgr = (KX_CollisionEventManager*) eventmgr;
//	m_resptable = collisionmgr->GetResponseTable();
	
//	m_solidHandle = m_sumoObj->getObjectHandle();

	m_colliders = new CListValue();
	
	KX_ClientObjectInfo *client_info = gameobj->getClientInfo();
	//client_info->m_gameobject = gameobj;
	//client_info->m_auxilary_info = NULL;
	client_info->m_sensors.push_back(this);
	
	m_physCtrl = gameobj->GetPhysicsController();
	BLI_assert( !gameobj->GetPhysicsController() || m_physCtrl );
	Init();
}

void KX_CollisionSensor::Init()
{
	m_bCollision = false;
	m_bTriggered = false;
	m_bLastTriggered = (m_invert)?true:false;
	m_bLastCount = 0;
	m_bColliderHash = m_bLastColliderHash = 0;
	m_hitObject =  NULL;
	m_reset = true;
}

KX_CollisionSensor::~KX_CollisionSensor()
{
	//DT_ClearObjectResponse(m_resptable,m_solidHandle);
	m_colliders->Release();
}

CValue* KX_CollisionSensor::GetReplica() 
{
	KX_CollisionSensor* replica = new KX_CollisionSensor(*this);
	replica->ProcessReplica();
	return replica;
}

void KX_CollisionSensor::ProcessReplica()
{
	SCA_ISensor::ProcessReplica();
	m_colliders = new CListValue();
	Init();
}

void	KX_CollisionSensor::ReParent(SCA_IObject* parent)
{
	KX_GameObject *gameobj = static_cast<KX_GameObject *>(parent);
	PHY_IPhysicsController *sphy = ((KX_GameObject*)parent)->GetPhysicsController();
	if (sphy)
		m_physCtrl = sphy;
	
//	m_solidHandle = m_sumoObj->getObjectHandle();
	KX_ClientObjectInfo *client_info = gameobj->getClientInfo();
	//client_info->m_gameobject = gameobj;
	//client_info->m_auxilary_info = NULL;
	
	client_info->m_sensors.push_back(this);
	SCA_ISensor::ReParent(parent);
}

void KX_CollisionSensor::RegisterSumo(KX_CollisionEventManager *collisionman)
{
	if (m_physCtrl)
	{
		if (collisionman->GetPhysicsEnvironment()->RequestCollisionCallback(m_physCtrl))
		{
			KX_ClientObjectInfo *client_info = static_cast<KX_ClientObjectInfo*>(m_physCtrl->GetNewClientInfo());
			if (client_info->isSensor())
				collisionman->GetPhysicsEnvironment()->AddSensor(m_physCtrl);
		}
	}
}
void KX_CollisionSensor::UnregisterSumo(KX_CollisionEventManager* collisionman)
{
	if (m_physCtrl)
	{
		if (collisionman->GetPhysicsEnvironment()->RemoveCollisionCallback(m_physCtrl))
		{
			// no more sensor on the controller, can remove it if it is a sensor object
			KX_ClientObjectInfo *client_info = static_cast<KX_ClientObjectInfo*>(m_physCtrl->GetNewClientInfo());
			if (client_info->isSensor())
				collisionman->GetPhysicsEnvironment()->RemoveSensor(m_physCtrl);
		}
	}
}

// this function is called only for sensor objects
// return true if the controller can collide with the object
bool	KX_CollisionSensor::BroadPhaseSensorFilterCollision(void*obj1,void*obj2)
{
	assert(obj1==m_physCtrl && obj2);

	KX_GameObject* myobj = (KX_GameObject*)GetParent();
	KX_GameObject* myparent = myobj->GetParent();
	KX_ClientObjectInfo *client_info = static_cast<KX_ClientObjectInfo*>(((PHY_IPhysicsController*)obj2)->GetNewClientInfo());
	KX_ClientObjectInfo *my_client_info = static_cast<KX_ClientObjectInfo*>(m_physCtrl->GetNewClientInfo());
	KX_GameObject* otherobj = ( client_info ? client_info->m_gameobject : NULL);

	// we can only check on persistent characteristic: m_link and m_suspended are not
	// good candidate because they are transient. That must be handled at another level
	if (!otherobj ||
		otherobj == myparent ||		// don't interact with our parent
		(my_client_info->m_type == KX_ClientObjectInfo::OBACTORSENSOR &&
		 client_info->m_type != KX_ClientObjectInfo::ACTOR))	// only with actor objects
		return false;
		
	bool found = m_touchedpropname.IsEmpty();
	if (!found)
	{
		if (m_bFindMaterial) {
			for (unsigned int i = 0; i < otherobj->GetMeshCount(); ++i) {
				RAS_MeshObject *meshObj = otherobj->GetMesh(i);
				for (unsigned int j = 0; j < meshObj->NumMaterials(); ++j) {
					found = strcmp(m_touchedpropname.ReadPtr(), meshObj->GetMaterialName(j).ReadPtr() + 2) == 0;
					if (found)
						break;
				}
			}
		}
		else {
			found = (otherobj->GetProperty(m_touchedpropname) != NULL);
		}
	}
	return found;
}

bool	KX_CollisionSensor::NewHandleCollision(void*object1,void*object2,const PHY_CollData* colldata)
{
//	KX_CollisionEventManager* toucheventmgr = (KX_CollisionEventManager*)m_eventmgr;
	KX_GameObject* parent = (KX_GameObject*)GetParent();

	// need the mapping from PHY_IPhysicsController to gameobjects now
	
	KX_ClientObjectInfo *client_info = static_cast<KX_ClientObjectInfo*> (object1 == m_physCtrl? 
					((PHY_IPhysicsController*)object2)->GetNewClientInfo():
					((PHY_IPhysicsController*)object1)->GetNewClientInfo());

	KX_GameObject* gameobj = ( client_info ? 
			client_info->m_gameobject : 
			NULL);
	
	// add the same check as in SCA_ISensor::Activate(), 
	// we don't want to record collision when the sensor is not active.
	if (m_links && !m_suspended &&
		gameobj && (gameobj != parent) && client_info->isActor())
	{
		
		bool found = m_touchedpropname.IsEmpty();
		bool hitMaterial = false;
		if (!found)
		{
			if (m_bFindMaterial) {
				for (unsigned int i = 0; i < gameobj->GetMeshCount(); ++i) {
					RAS_MeshObject *meshObj = gameobj->GetMesh(i);
					for (unsigned int j = 0; j < meshObj->NumMaterials(); ++j) {
						found = strcmp(m_touchedpropname.ReadPtr(), meshObj->GetMaterialName(j).ReadPtr() + 2) == 0;
						if (found) {
							hitMaterial = true;
							break;
						}
					}
				}
			}
			else {
				found = (gameobj->GetProperty(m_touchedpropname) != NULL);
			}
		}
		if (found)
		{
			if (!m_colliders->SearchValue(gameobj)) {
				m_colliders->Add(gameobj->AddRef());
				
				if (m_bCollisionPulse)
					m_bColliderHash += (uint_ptr)(static_cast<void *>(&gameobj));
			}
			m_bTriggered = true;
			m_hitObject = gameobj;
			m_hitMaterial = hitMaterial;
		}
		
	} 
	return false; // was DT_CONTINUE but this was defined in sumo as false.
}

#ifdef WITH_PYTHON

/* ------------------------------------------------------------------------- */
/* Python functions                                                          */
/* ------------------------------------------------------------------------- */
/* Integration hooks ------------------------------------------------------- */
PyTypeObject KX_CollisionSensor::Type = {
	PyVarObject_HEAD_INIT(NULL, 0)
	"KX_CollisionSensor",
	sizeof(PyObjectPlus_Proxy),
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
	&SCA_ISensor::Type,
	0,0,0,0,0,0,
	py_base_new
};

PyMethodDef KX_CollisionSensor::Methods[] = {
	{NULL,NULL} //Sentinel
};

PyAttributeDef KX_CollisionSensor::Attributes[] = {
	KX_PYATTRIBUTE_STRING_RW("propName",0,MAX_PROP_NAME,false,KX_CollisionSensor,m_touchedpropname),
	KX_PYATTRIBUTE_BOOL_RW("useMaterial",KX_CollisionSensor,m_bFindMaterial),
	KX_PYATTRIBUTE_BOOL_RW("usePulseCollision",KX_CollisionSensor,m_bCollisionPulse),
	KX_PYATTRIBUTE_STRING_RO("hitMaterial", KX_CollisionSensor, m_hitMaterial),
	KX_PYATTRIBUTE_RO_FUNCTION("hitObject", KX_CollisionSensor, pyattr_get_object_hit),
	KX_PYATTRIBUTE_RO_FUNCTION("hitObjectList", KX_CollisionSensor, pyattr_get_object_hit_list),
	{ NULL }	//Sentinel
};

/* Python API */

PyObject *KX_CollisionSensor::pyattr_get_object_hit(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_CollisionSensor* self = static_cast<KX_CollisionSensor*>(self_v);
	
	if (self->m_hitObject)
		return self->m_hitObject->GetProxy();
	else
		Py_RETURN_NONE;
}

PyObject *KX_CollisionSensor::pyattr_get_object_hit_list(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_CollisionSensor* self = static_cast<KX_CollisionSensor*>(self_v);
	return self->m_colliders->GetProxy();
}

#endif

/* eof */
