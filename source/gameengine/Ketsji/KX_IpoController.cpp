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
 * Scenegraph controller for ipos.
 */

/** \file gameengine/Ketsji/KX_IpoController.cpp
 *  \ingroup ketsji
 */

#include "KX_IpoController.h"
#include "KX_GameObject.h"
#include "PHY_IPhysicsController.h"
#include "DNA_ipo_types.h"
#include "BLI_math.h"

KX_IpoController::KX_IpoController()
	:m_ipo_as_force(false),
	m_ipo_add(false),
	m_ipo_local(false),
	m_ipo_start_initialized(false),
	m_ipo_start_euler(mt::zero3),
	m_ipo_euler_initialized(false)
{
	for (int i = 0; i < KX_MAX_IPO_CHANNELS; i++) {
		m_ipo_channels_active[i] = false;
	}
}

void KX_IpoController::SetOption(SG_ControllerOption option, bool value)
{
	m_modified = true;

	switch (option) {
		case SG_CONTR_IPO_IPO_AS_FORCE:
		{
			m_ipo_as_force = value;
			break;
		}
		case SG_CONTR_IPO_IPO_ADD:
		{
			m_ipo_add = value;
			break;
		}
		case SG_CONTR_IPO_RESET:
		{
			if (m_ipo_start_initialized && value) {
				m_ipo_start_initialized = false;
			}
			break;
		}
		case SG_CONTR_IPO_LOCAL:
		{
			m_ipo_local = true;
			break;
		}
		default:
			; /* just ignore the rest */
	}
}

bool KX_IpoController::Update(SG_Node *node)
{
	if (!SG_Controller::Update(node)) {
		return false;
	}

	KX_GameObject *obj = static_cast<KX_GameObject *>(node->GetObject());

	//initialization on the first frame of the IPO
	if (!m_ipo_start_initialized) {
		m_ipo_start_point = node->GetLocalPosition();
		m_ipo_start_orient = node->GetLocalOrientation();
		m_ipo_start_scale = node->GetLocalScale();
		m_ipo_start_initialized = true;

		if (!m_ipo_euler_initialized) {
			// do it only once to avoid angle discontinuities
			m_ipo_start_euler = m_ipo_start_orient.GetEuler();
			m_ipo_euler_initialized = true;
		}
	}

	//modifies position?
	if (m_ipo_channels_active[OB_LOC_X]  || m_ipo_channels_active[OB_LOC_Y]  || m_ipo_channels_active[OB_LOC_Z] ||
	    m_ipo_channels_active[OB_DLOC_X] || m_ipo_channels_active[OB_DLOC_Y] || m_ipo_channels_active[OB_DLOC_Z]) {
		if (m_ipo_as_force) {
			if (obj->GetPhysicsController()) {
				const mt::vec3 vec = m_ipo_local ?
				                     node->GetWorldOrientation() * m_ipo_xform.GetPosition() :
				                     m_ipo_xform.GetPosition();
				obj->GetPhysicsController()->ApplyForce(vec, false);
			}
		}
		else {
			// Local ipo should be defined with the nodeject position at (0,0,0)
			// Local transform is applied to the nodeject based on initial position
			mt::vec3 newPosition = mt::zero3;

			if (!m_ipo_add) {
				newPosition = node->GetLocalPosition();
			}
			//apply separate IPO channels if there is any data in them
			//Loc and dLoc act by themselves or are additive
			for (unsigned short i = 0; i < 3; ++i) {
				const mt::vec3& loc = m_ipo_xform.GetPosition();
				const mt::vec3& dloc = m_ipo_xform.GetDeltaPosition();

				const bool delta = m_ipo_channels_active[OB_DLOC_X + i];

				if (m_ipo_channels_active[OB_LOC_X + i]) {
					newPosition[i] = (delta ? loc[i] + dloc[i] : loc[i]);
				}
				else if (delta) {
					newPosition[i] = (((!m_ipo_add) ? m_ipo_start_point[i] : 0.0f) + dloc[i]);
				}
			}

			if (m_ipo_add) {
				if (m_ipo_local) {
					newPosition = m_ipo_start_point + m_ipo_start_scale * (m_ipo_start_orient * newPosition);
				}
				else {
					newPosition = m_ipo_start_point + newPosition;
				}
			}

			obj->NodeSetLocalPosition(newPosition);
		}
	}
	//modifies orientation?
	if (m_ipo_channels_active[OB_ROT_X]  || m_ipo_channels_active[OB_ROT_Y]  || m_ipo_channels_active[OB_ROT_Z] ||
	    m_ipo_channels_active[OB_DROT_X] || m_ipo_channels_active[OB_DROT_Y] || m_ipo_channels_active[OB_DROT_Z]) {
		if (m_ipo_as_force) {
			obj->ApplyTorque(m_ipo_local ?
	                           node->GetWorldOrientation() * m_ipo_xform.GetEulerAngles() :
	                           m_ipo_xform.GetEulerAngles(), false);
		}
		else if (m_ipo_add) {
			// Delta euler angles.
			mt::vec3 angles = mt::zero3;

			for (unsigned short i = 0; i < 3; ++i) {
				if (m_ipo_channels_active[OB_ROT_X + i]) {
					angles[i] += m_ipo_xform.GetEulerAngles()[i];
				}
				if (m_ipo_channels_active[OB_DROT_X + i]) {
					angles[i] += m_ipo_xform.GetDeltaEulerAngles()[i];
				}
			}

			mt::mat3 rotation(angles);
			if (m_ipo_local) {
				rotation = m_ipo_start_orient * rotation;
			}
			else {
				rotation = rotation * m_ipo_start_orient;
			}

			obj->NodeSetLocalOrientation(rotation);
		}
		else if (m_ipo_channels_active[OB_ROT_X] || m_ipo_channels_active[OB_ROT_Y] || m_ipo_channels_active[OB_ROT_Z]) {
			// assume all channel absolute
			// All 3 channels should be specified but if they are not, we will take
			// the value at the start of the game to avoid angle sign reversal
			mt::vec3 angles(m_ipo_start_euler);

			for (unsigned short i = 0; i < 3; ++i) {
				const mt::vec3& eul = m_ipo_xform.GetEulerAngles();
				const mt::vec3& deul = m_ipo_xform.GetDeltaEulerAngles();

				const bool delta = m_ipo_channels_active[OB_DROT_X + i];

				if (m_ipo_channels_active[OB_ROT_X + i]) {
					angles[i] = (delta ? (eul[i] + deul[i]) : eul[i]);
				}
				else if (delta) {
					angles[i] += deul[i];
				}
			}

			obj->NodeSetLocalOrientation(mt::mat3(angles));
		}
		else {
			mt::vec3 angles = mt::zero3;

			for (unsigned short i = 0; i < 3; ++i) {
				if (m_ipo_channels_active[OB_DROT_X + i]) {
					angles[i] = m_ipo_xform.GetDeltaEulerAngles()[i];
				}
			}

			// dRot are always local
			const mt::mat3 rotation = m_ipo_start_orient * mt::mat3(angles);

			obj->NodeSetLocalOrientation(rotation);
		}
	}
	//modifies scale?
	if (m_ipo_channels_active[OB_SIZE_X] || m_ipo_channels_active[OB_SIZE_Y] || m_ipo_channels_active[OB_SIZE_Z] ||
	    m_ipo_channels_active[OB_DSIZE_X] || m_ipo_channels_active[OB_DSIZE_Y] || m_ipo_channels_active[OB_DSIZE_Z]) {
		//default is no scale change
		mt::vec3 newScale = mt::one3;
		if (!m_ipo_add) {
			newScale = node->GetLocalScale();
		}

		for (unsigned short i = 0; i < 3; ++i) {
			const mt::vec3& scale = m_ipo_xform.GetScaling();
			const mt::vec3& dscale = m_ipo_xform.GetDeltaScaling();

			const bool delta = m_ipo_channels_active[OB_DSIZE_X + i];

			if (m_ipo_channels_active[OB_SIZE_X + i]) {
				newScale[i] = (delta ? (scale[i] + dscale[i]) : scale[i]);
			}
			else if (delta) {
				newScale[i] = (dscale[i] + ((!m_ipo_add) ? m_ipo_start_scale[i] : 0.0f));
			}
		}

		if (m_ipo_add) {
			newScale = m_ipo_start_scale * newScale;
		}

		obj->NodeSetLocalScale(newScale);
	}

	return true;
}
