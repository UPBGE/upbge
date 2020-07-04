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

/** \file gameengine/Ketsji/KX_WorldIpoController.cpp
 *  \ingroup ketsji
 */


#include "KX_WorldIpoController.h"
#include "KX_WorldInfo.h"
#include "KX_Globals.h"
#include "KX_Scene.h"

KX_WorldIpoController::KX_WorldIpoController(KX_Scene *scene) :
	m_modify_mist_start(false),
	m_modify_mist_dist(false),
	m_modify_mist_intensity(false),
	m_modify_horizon_color(false),
	m_modify_zenith_color(false),
	m_modify_ambient_color(false),
	m_kxscene(scene)
{
}

KX_WorldIpoController::~KX_WorldIpoController()
{
}

bool KX_WorldIpoController::Update(SG_Node *node)
{
	if (!SG_Controller::Update(node)) {
		return false;
	}

	KX_WorldInfo *world = m_kxscene->GetWorldInfo();

	if (m_modify_mist_start) {
		world->setMistStart(m_mist_start);
	}

	if (m_modify_mist_dist) {
		world->setMistDistance(m_mist_dist);
	}

	if (m_modify_mist_intensity) {
		world->setMistIntensity(m_mist_intensity);
	}

	if (m_modify_horizon_color) {
		world->setHorizonColor(mt::vec4(m_hori_rgb[0], m_hori_rgb[1], m_hori_rgb[2], 1.0f));
		world->setMistColor(m_hori_rgb);
	}

	if (m_modify_zenith_color) {
		world->setZenithColor(mt::vec4(m_zeni_rgb[0], m_zeni_rgb[1], m_zeni_rgb[2], 1.0f));
	}

	if (m_modify_ambient_color) {
		world->setAmbientColor(m_ambi_rgb);
	}

	return true;
}
