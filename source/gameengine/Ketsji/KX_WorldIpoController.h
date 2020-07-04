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

/** \file KX_WorldIpoController.h
 *  \ingroup ketsji
 */

#ifndef __KX_WORLDIPOCONTROLLER_H__
#define __KX_WORLDIPOCONTROLLER_H__

#include "SG_Controller.h"
#include "SG_Node.h"
#include "SG_Interpolator.h"

class KX_Scene;

class KX_WorldIpoController : public SG_Controller, public mt::SimdClassAllocator
{
public:
	float           m_mist_start;
	float           m_mist_dist;
	float           m_mist_intensity;
	mt::vec3 m_hori_rgb;
	mt::vec3 m_zeni_rgb;
	mt::vec3 m_ambi_rgb;

private:
	unsigned short		m_modify_mist_start	 : 1;
	unsigned short  	m_modify_mist_dist 	 : 1;
	unsigned short		m_modify_mist_intensity	: 1;
	unsigned short		m_modify_horizon_color	: 1;
	unsigned short		m_modify_zenith_color : 1;
	unsigned short		m_modify_ambient_color	: 1;

	KX_Scene *m_kxscene;

public:
	KX_WorldIpoController(KX_Scene* scene);

	virtual ~KX_WorldIpoController();

	virtual bool Update(SG_Node *node);

	void	SetModifyMistStart(bool modify) {
		m_modify_mist_start = modify;
	}

	void	SetModifyMistDist(bool modify) {
		m_modify_mist_dist = modify;
	}

	void	SetModifyMistIntensity(bool modify) {
		m_modify_mist_intensity = modify;
	}

	void	SetModifyHorizonColor(bool modify) {
		m_modify_horizon_color = modify;
	}

	void	SetModifyZenithColor(bool modify) {
		m_modify_zenith_color = modify;
	}

	void	SetModifyAmbientColor(bool modify) {
		m_modify_ambient_color = modify;
	}
};

#endif  /* __KX_WORLDIPOCONTROLLER_H__ */
