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

/** \file BL_DeformableGameObject.h
 *  \ingroup bgeconv
 */

#ifndef __BL_DEFORMABLEGAMEOBJECT_H__
#define __BL_DEFORMABLEGAMEOBJECT_H__

#ifdef _MSC_VER
#  pragma warning (disable:4786) // get rid of stupid stl-visual compiler debug warning
#endif

#include "DNA_mesh_types.h"
#include "KX_GameObject.h"
#include "RAS_Deformer.h"
#include <vector>

struct Key;

class BL_DeformableGameObject : public KX_GameObject  
{
public:
	CValue*		GetReplica();

	double GetLastFrame ()
	{
		return m_lastframe;
	}
	Object* GetBlendObject()
	{
		return m_blendobj;
	}
	virtual void Relink(std::map<SCA_IObject *, SCA_IObject *>& map)
	{
		if (m_pDeformer)
			m_pDeformer->Relink(map);
		KX_GameObject::Relink(map);
	};
	void ProcessReplica();

	BL_DeformableGameObject(Object* blendobj, void* sgReplicationInfo, SG_Callbacks callbacks) :
		KX_GameObject(sgReplicationInfo,callbacks),
		m_pDeformer(nullptr),
		m_lastframe(0.0),
		m_blendobj(blendobj),
		m_activePriority(9999)
	{
	};
	virtual ~BL_DeformableGameObject();
	bool SetActiveAction(short priority, double curtime);

	bool GetShape(std::vector<float> &shape);
	
	virtual void	SetDeformer(class RAS_Deformer* deformer);
	virtual class RAS_Deformer* GetDeformer()
	{
		return m_pDeformer;
	}
	virtual bool IsDeformable() const
	{
		return true;
	}

public:
	
protected:
	
	RAS_Deformer		*m_pDeformer;

	double		m_lastframe;
	Object*		m_blendobj;
	short		m_activePriority;
};

#endif  /* __BL_DEFORMABLEGAMEOBJECT_H__ */
