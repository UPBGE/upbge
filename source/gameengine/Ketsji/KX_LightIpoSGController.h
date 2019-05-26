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

/** \file KX_LightIpoSGController.h
 *  \ingroup ketsji
 */

#ifndef __KX_LIGHTIPOSGCONTROLLER_H__
#define __KX_LIGHTIPOSGCONTROLLER_H__

#include "SG_Controller.h"
#include "SG_Node.h"

#include "SG_Interpolator.h"

class RAS_ILightObject;

class KX_LightIpoSGController : public SG_Controller
{
public:
	float           m_energy;
	mt::vec3 m_col_rgb;
	float           m_dist;

private:
	unsigned short  	m_modify_energy 	 : 1;
	unsigned short	    m_modify_color       : 1;
	unsigned short		m_modify_dist    	 : 1;

public:
	KX_LightIpoSGController() : 
				m_modify_energy(false),
				m_modify_color(false),
				m_modify_dist(false)
		{}

	virtual ~KX_LightIpoSGController() = default;

	virtual bool Update(SG_Node *node);

	void	SetModifyEnergy(bool modify) {
		m_modify_energy = modify;
	}

	void	SetModifyColor(bool modify) {
		m_modify_color = modify;
	}

	void	SetModifyDist(bool modify) {
		m_modify_dist = modify;
	}
};

#endif  /* __KX_LIGHTIPOSGCONTROLLER_H__ */
