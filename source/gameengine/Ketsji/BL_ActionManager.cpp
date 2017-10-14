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
 * Contributor(s): Mitchell Stokes.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file BL_ActionManager.cpp
 *  \ingroup ketsji
 */

#include "BL_Action.h"
#include "BL_ActionManager.h"
#include "DNA_ID.h"

#define IS_TAGGED(_id) ((_id) && (((ID *)_id)->tag & LIB_TAG_DOIT))

BL_ActionManager::BL_ActionManager(class KX_GameObject *obj) :
	m_obj(obj),
	m_suspended(false)
{
}

BL_ActionManager::~BL_ActionManager()
{
	BL_ActionMap::iterator it;

	for (it = m_layers.begin(); it != m_layers.end(); it++) {
		delete it->second;
	}

	m_layers.clear();
}

BL_Action *BL_ActionManager::GetAction(short layer)
{
	BL_ActionMap::iterator it = m_layers.find(layer);

	return (it != m_layers.end()) ? it->second : 0;
}

float BL_ActionManager::GetActionFrame(short layer)
{
	BL_Action *action = GetAction(layer);

	return action ? action->GetFrame() : 0.f;
}

const std::string BL_ActionManager::GetActionName(short layer)
{
	BL_Action *action = GetAction(layer);
	return action ? action->GetName() : "";
}

void BL_ActionManager::SetActionFrame(short layer, float frame)
{
	BL_Action *action = GetAction(layer);

	if (action) {
		action->SetFrame(frame);
	}
}

struct bAction *BL_ActionManager::GetCurrentAction(short layer)
{
	BL_Action *action = GetAction(layer);

	return action ? action->GetAction() : 0;
}

void BL_ActionManager::SetPlayMode(short layer, short mode)
{
	BL_Action *action = GetAction(layer);

	if (action) {
		action->SetPlayMode(mode);
	}
}

bool BL_ActionManager::PlayAction(const std::string& name,
                                  float start,
                                  float end,
                                  short layer,
                                  short priority,
                                  float blendin,
                                  short play_mode,
                                  float layer_weight,
                                  short ipo_flags,
                                  float playback_speed,
                                  short blend_mode)
{
	// Only this method will create layer if non-existent
	BL_Action *action = GetAction(layer);
	if (!action) {
		action = new BL_Action(m_obj);
		m_layers[layer] = action;
	}

	// Disable layer blending on the first layer
	if (layer == 0) {
		layer_weight = -1.f;
	}

	return action->Play(name, start, end, priority, blendin, play_mode, layer_weight, ipo_flags, playback_speed, blend_mode);
}

void BL_ActionManager::StopAction(short layer)
{
	BL_Action *action = GetAction(layer);

	if (action) {
		m_layers.erase(layer);
		delete action;
	}
}

void BL_ActionManager::RemoveTaggedActions()
{
	for (BL_ActionMap::iterator it = m_layers.begin(); it != m_layers.end(); ) {
		if (IS_TAGGED(it->second->GetAction())) {
			delete it->second;
			it = m_layers.erase(it);
		}
		else {
			++it;
		}
	}
}

bool BL_ActionManager::IsActionDone(short layer)
{
	BL_Action *action = GetAction(layer);

	return action ? action->IsDone() : true;
}

void BL_ActionManager::Suspend()
{
	m_suspended = true;
}

void BL_ActionManager::Resume()
{
	m_suspended = false;
}

bool BL_ActionManager::IsSuspended() const
{
	return m_suspended;
}

void BL_ActionManager::Update(float curtime, bool applyToObject, bool redundant)
{
	for (const auto& pair : m_layers) {
		pair.second->Update(curtime, applyToObject, redundant);
	}

	for (const auto& pair : m_layers) {
		pair.second->UpdateIPOs();
	}
}
