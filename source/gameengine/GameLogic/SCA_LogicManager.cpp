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
 * Regulates the top-level logic behavior for one scene.
 */

/** \file gameengine/GameLogic/SCA_LogicManager.cpp
 *  \ingroup gamelogic
 */

#include "EXP_Value.h"
#include "SCA_LogicManager.h"
#include "SCA_ISensor.h"
#include "SCA_IController.h"
#include "SCA_IActuator.h"
#include "SCA_EventManager.h"
#include "SCA_PythonController.h"

#include "CM_Map.h"

SCA_LogicManager::SCA_LogicManager()
{
}



SCA_LogicManager::~SCA_LogicManager()
{
	BLI_assert(m_activeActuators.Empty());
}

void SCA_LogicManager::RegisterEventManager(SCA_EventManager *eventmgr)
{
	m_eventmanagers.emplace_back(eventmgr);
}



void SCA_LogicManager::RegisterGameObjectName(const std::string& gameobjname,
                                              EXP_Value *gameobj)
{
	std::string mn = gameobjname;
	m_mapStringToGameObjects[mn] = gameobj;
}

void SCA_LogicManager::UnregisterGameObjectName(const std::string& gameobjname)
{
	m_mapStringToGameObjects.erase(gameobjname);
}


void SCA_LogicManager::RegisterGameMeshName(const std::string& gamemeshname, void *blendobj)
{
	std::string mn = gamemeshname;
	m_map_gamemeshname_to_blendobj[mn] = blendobj;
}



void SCA_LogicManager::RegisterGameObj(void *blendobj, EXP_Value *gameobj)
{
	m_map_blendobj_to_gameobj[blendobj] = gameobj;
}

void SCA_LogicManager::UnregisterGameObj(void *blendobj, EXP_Value *gameobj)
{
	std::map<void *, EXP_Value *>::iterator it = m_map_blendobj_to_gameobj.find(blendobj);
	if (it != m_map_blendobj_to_gameobj.end() && it->second == gameobj) {
		m_map_blendobj_to_gameobj.erase(it);
	}
}

EXP_Value *SCA_LogicManager::GetGameObjectByName(const std::string& gameobjname)
{
	std::string mn = gameobjname;
	return m_mapStringToGameObjects[mn];
}


EXP_Value *SCA_LogicManager::FindGameObjByBlendObj(void *blendobj)
{
	return m_map_blendobj_to_gameobj[blendobj];
}



void *SCA_LogicManager::FindBlendObjByGameMeshName(const std::string& gamemeshname)
{
	std::string mn = gamemeshname;
	return m_map_gamemeshname_to_blendobj[mn];
}



void SCA_LogicManager::RemoveSensor(SCA_ISensor *sensor)
{
	sensor->UnlinkAllControllers();
	sensor->UnregisterToManager();
}

void SCA_LogicManager::RemoveController(SCA_IController *controller)
{
	controller->UnlinkAllSensors();
	controller->UnlinkAllActuators();
	controller->Deactivate();
}


void SCA_LogicManager::RemoveActuator(SCA_IActuator *actuator)
{
	actuator->UnlinkAllControllers();
	actuator->Deactivate();
	actuator->SetActive(false);
}



void SCA_LogicManager::RegisterToSensor(SCA_IController *controller, SCA_ISensor *sensor)
{
	sensor->LinkToController(controller);
	controller->LinkToSensor(sensor);
}



void SCA_LogicManager::RegisterToActuator(SCA_IController *controller, SCA_IActuator *actua)
{
	actua->LinkToController(controller);
	controller->LinkToActuator(actua);
}



void SCA_LogicManager::BeginFrame(double curtime, double fixedtime)
{
	for (std::unique_ptr<SCA_EventManager>& mgr : m_eventmanagers) {
		mgr->NextFrame(curtime, fixedtime);
	}

	for (SG_QList *obj = (SG_QList *)m_triggeredControllerSet.Remove();
	     obj != nullptr;
	     obj = (SG_QList *)m_triggeredControllerSet.Remove())
	{
		for (SCA_IController *contr = (SCA_IController *)obj->QRemove();
		     contr != nullptr;
		     contr = (SCA_IController *)obj->QRemove())
		{
			contr->Trigger(this);
			contr->ClrJustActivated();
		}
	}
}



void SCA_LogicManager::UpdateFrame(double curtime)
{
	for (std::unique_ptr<SCA_EventManager>& mgr : m_eventmanagers) {
		mgr->UpdateFrame();
	}

	SG_DList::iterator<SG_QList> io(m_activeActuators);
	for (io.begin(); !io.end(); )
	{
		SG_QList *ahead = *io;
		// increment now so that we can remove the current element
		++io;
		SG_QList::iterator<SCA_IActuator> ia(*ahead);
		for (ia.begin(); !ia.end(); )
		{
			SCA_IActuator *actua = *ia;
			// increment first to allow removal of inactive actuators.
			++ia;
			if (!actua->Update(curtime)) {
				// this actuator is not active anymore, remove
				actua->QDelink();
				actua->SetActive(false);
			}
			else if (actua->IsNoLink()) {
				// This actuator has no more links but it still active
				// make sure it will get a negative event on next frame to stop it
				// Do this check after Update() rather than before to make sure
				// that all the actuators that are activated at same time than a state
				// actuator have a chance to execute.
				bool event = false;
				actua->RemoveAllEvents();
				actua->AddEvent(event);
			}
		}
		if (ahead->QEmpty()) {
			// no more active controller, remove from main list
			ahead->Delink();
		}
	}
}



void *SCA_LogicManager::GetActionByName(const std::string& actname)
{
	const auto it = m_mapStringToActions.find(actname);
	if (it != m_mapStringToActions.end()) {
		return it->second;
	}

	return nullptr;
}

void *SCA_LogicManager::GetMeshByName(const std::string& meshname)
{
	const auto it = m_mapStringToMeshes.find(meshname);
	if (it != m_mapStringToMeshes.end()) {
		return it->second;
	}

	return nullptr;
}

void SCA_LogicManager::RegisterMeshName(const std::string& meshname, void *mesh)
{
	m_mapStringToMeshes[meshname] = mesh;
}

void SCA_LogicManager::UnregisterMeshName(const std::string& meshname, void *mesh)
{
	m_mapStringToMeshes.erase(meshname);
}

void SCA_LogicManager::UnregisterMesh(void *mesh)
{
	CM_MapRemoveIfItemFound(m_mapStringToMeshes, mesh);
}

void SCA_LogicManager::RegisterActionName(const std::string& actname, void *action)
{
	std::string an = actname;
	m_mapStringToActions[an] = action;
}

void SCA_LogicManager::UnregisterAction(void *action)
{
	CM_MapRemoveIfItemFound(m_mapStringToActions, action);
}

void SCA_LogicManager::EndFrame()
{
	for (std::unique_ptr<SCA_EventManager>& emgr : m_eventmanagers) {
		emgr->EndFrame();
	}
}


void SCA_LogicManager::AddTriggeredController(SCA_IController *controller, SCA_ISensor *sensor)
{
	controller->Activate(m_triggeredControllerSet);

#ifdef WITH_PYTHON

	// so that the controller knows which sensor has activited it
	// only needed for python controller
	// Note that this is safe even if the controller is subclassed.
	if (controller->GetType() == &SCA_PythonController::Type) {
		SCA_PythonController *pythonController = (SCA_PythonController *)controller;
		pythonController->AddTriggeredSensor(sensor);
	}
#endif
}

SCA_EventManager *SCA_LogicManager::FindEventManager(int eventmgrtype)
{
	// find an eventmanager of a certain type
	SCA_EventManager *eventmgr = nullptr;

	for (std::unique_ptr<SCA_EventManager>& emgr : m_eventmanagers) {
		if (emgr->GetType() == eventmgrtype) {
			eventmgr = emgr.get();
			break;
		}
	}
	return eventmgr;
}
