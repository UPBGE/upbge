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

/** \file KX_IPOTransform.h
 *  \ingroup ketsji
 *  \brief An abstract object you can move around in a 3d world, and has some logic
 */

#ifndef __KX_IPOTRANSFORM_H__
#define __KX_IPOTRANSFORM_H__

#include "mathfu.h"

class KX_IPOTransform {
public:
	KX_IPOTransform() :
		m_position(mt::zero3),
		m_eulerAngles(mt::zero3),
		m_scaling(mt::one3),
		m_deltaPosition(mt::zero3),
		m_deltaEulerAngles(mt::zero3),
		m_deltaScaling(mt::zero3)
		{}

	mt::vec3&	         GetPosition()          { return m_position; 	}
	mt::vec3&          GetEulerAngles()       { return m_eulerAngles;	}
	mt::vec3&          GetScaling()           { return m_scaling;	}

	const mt::vec3&	 GetPosition()    const { return m_position; 	}
	const mt::vec3&    GetEulerAngles() const { return m_eulerAngles;	}
	const mt::vec3&    GetScaling()     const { return m_scaling;	}
	
	mt::vec3&          GetDeltaPosition()     { return m_deltaPosition; }
	mt::vec3&          GetDeltaEulerAngles()  { return m_deltaEulerAngles; }
	mt::vec3&          GetDeltaScaling()      { return m_deltaScaling; }
	
	void SetPosition(const mt::vec3& pos)      { m_position = pos; 	}
	void SetEulerAngles(const mt::vec3& eul)  { m_eulerAngles = eul;	}
	void SetScaling(const mt::vec3& scaling)  { m_scaling = scaling;	}
	
	void ClearDeltaStuff() { 
		m_deltaPosition = mt::zero3;
		m_deltaEulerAngles = mt::zero3;
		m_deltaScaling = mt::zero3;
	}

protected:
	mt::vec3              m_position;
	mt::vec3             m_eulerAngles;
	mt::vec3             m_scaling;
	mt::vec3             m_deltaPosition;
	mt::vec3             m_deltaEulerAngles;
	mt::vec3             m_deltaScaling;
};

#endif

