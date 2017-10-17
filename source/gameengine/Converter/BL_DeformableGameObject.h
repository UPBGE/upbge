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

#include "KX_GameObject.h"
#include <vector>

class RAS_Deformer;
struct Key;

class BL_DeformableGameObject : public KX_GameObject
{
private:
	RAS_Deformer *m_pDeformer;

	double m_lastframe;
	short m_activePriority;

public:
	BL_DeformableGameObject(void *sgReplicationInfo, SG_Callbacks callbacks);
	virtual ~BL_DeformableGameObject();

	virtual EXP_Value *GetReplica();
	void ProcessReplica();

	virtual void Relink(std::map<SCA_IObject *, SCA_IObject *>& map);

	double GetLastFrame() const;

	bool SetActiveAction(short priority, double curtime);
	bool GetShape(std::vector<float> &shape);

	void SetDeformer(RAS_Deformer *deformer);
	virtual RAS_Deformer *GetDeformer();
	virtual bool IsDeformable() const;

	virtual void LoadDeformer();
};

#endif  /* __BL_DEFORMABLEGAMEOBJECT_H__ */
