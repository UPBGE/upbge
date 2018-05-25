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

/** \file KX_IpoController.h
 *  \ingroup ketsji
 */

#ifndef __KX_IPO_SGCONTROLLER_H__
#define __KX_IPO_SGCONTROLLER_H__

#include "SG_Controller.h"
#include "SG_Node.h"

#include "KX_IpoTransform.h"
#include "SG_Interpolator.h"

#define KX_MAX_IPO_CHANNELS 19	//note- [0] is not used

class KX_IpoController : public SG_Controller, public mt::SimdClassAllocator
{
	KX_IpoTransform m_ipo_xform;

	/** Flag for each IPO channel that can be applied to a game object */
	bool m_ipo_channels_active[KX_MAX_IPO_CHANNELS];

	/** Interpret the ipo as a force rather than a displacement? */
	bool m_ipo_as_force;

	/** Add Ipo curve to current loc/rot/scale */
	bool m_ipo_add;

	/** Ipo must be applied in local coordinate rather than in global coordinates (used for force and Add mode)*/
	bool m_ipo_local;

	/** Location of the object when the IPO is first fired (for local transformations) */
	mt::vec3 m_ipo_start_point;

	/** Orientation of the object when the IPO is first fired (for local transformations) */
	mt::mat3 m_ipo_start_orient;

	/** Scale of the object when the IPO is first fired (for local transformations) */
	mt::vec3 m_ipo_start_scale;

	/** if IPO initial position has been set for local normal IPO */
	bool m_ipo_start_initialized;

	/** Euler angles at the start of the game, needed for incomplete ROT Ipo curves */
	mt::vec3 m_ipo_start_euler;

	/** true is m_ipo_start_euler has been initialized */
	bool m_ipo_euler_initialized;

public:
	KX_IpoController();
	virtual ~KX_IpoController() = default;

	virtual void SetOption(SG_ControllerOption option, bool value);

	void SetIPOChannelActive(int index, bool value) {
		//indexes found in makesdna\DNA_ipo_types.h
		m_ipo_channels_active[index] = value;
	}

	KX_IpoTransform &GetIPOTransform()
	{
		return m_ipo_xform;
	}

	virtual bool Update(SG_Node *node);
};

#endif  /* __KX_IPO_SGCONTROLLER_H__ */
