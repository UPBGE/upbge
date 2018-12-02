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
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Ketsji/KX_MaterialIpoController.cpp
 *  \ingroup ketsji
 */

#include "KX_MaterialIpoController.h"
#include "KX_GameObject.h"

#include "RAS_IMaterial.h"

bool KX_MaterialIpoController::Update(SG_Node *node)
{
	if (!SG_Controller::Update(node)) {
		return false;
	}

	m_material->UpdateIPO(m_rgba, m_specrgb, m_hard, m_spec, m_ref, m_emit, m_ambient, m_alpha, m_specAlpha, m_roughness_bsdf,  m_metallic_bsdf);

	return true;
}
