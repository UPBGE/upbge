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

/** \file gameengine/GameLogic/SCA_TimeEventManager.cpp
 *  \ingroup gamelogic
 */

#ifdef _MSC_VER
/* This warning tells us about truncation of __long__ stl-generated names.
 * It can occasionally cause DevStudio to have internal compiler warnings. */
#  pragma warning(disable:4786)
#endif

#include "SCA_TimeEventManager.h"

#include "SCA_LogicManager.h"
#include "EXP_FloatValue.h"

#include "CM_List.h"

SCA_TimeEventManager::SCA_TimeEventManager(SCA_LogicManager *logicmgr)
	:SCA_EventManager(nullptr, TIME_EVENTMGR)
{
}

SCA_TimeEventManager::~SCA_TimeEventManager()
{
	for (EXP_Value *prop : m_timevalues) {
		prop->Release();
	}
}

void SCA_TimeEventManager::NextFrame(double curtime, double fixedtime)
{
	if (m_timevalues.empty() && fixedtime <= 0.0) {
		return;
	}

	for (EXP_Value *prop : m_timevalues) {
		EXP_FloatValue *floatProp = static_cast<EXP_FloatValue *>(prop); // TODO store float prop directly.
		floatProp->SetValue(floatProp->GetValue() + fixedtime);
	}
}

void SCA_TimeEventManager::AddTimeProperty(EXP_Value *timeval)
{
	timeval->AddRef();
	m_timevalues.push_back(timeval);
}

void SCA_TimeEventManager::RemoveTimeProperty(EXP_Value *timeval)
{
	CM_ListRemoveIfFound(m_timevalues, timeval);
}

std::vector<EXP_Value *> SCA_TimeEventManager::GetTimeValues()
{
	return m_timevalues;
}

