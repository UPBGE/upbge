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

/** \file BL_ShapeDeformer.h
 *  \ingroup bgeconv
 */

#ifndef __BL_SHAPEDEFORMER_H__
#define __BL_SHAPEDEFORMER_H__

#ifdef _MSC_VER
#  pragma warning (disable:4786)  /* get rid of stupid stl-visual compiler debug warning */
#endif

#include "BL_SkinDeformer.h"
#include <vector>

struct Object;
struct Key;
class RAS_Mesh;

class BL_ShapeDeformer : public BL_SkinDeformer
{
public:
	BL_ShapeDeformer(KX_GameObject *gameobj,
					 Object *bmeshobj_old,
					 Object *bmeshobj_new,
					 RAS_Mesh *mesh,
					 BL_ArmatureObject *arma);

	virtual ~BL_ShapeDeformer();

	bool UpdateInternal(bool recalcNormal);
	virtual bool Update();
	bool LoadShapeDrivers(KX_GameObject *parent);
	bool ExecuteShapeDrivers();

	Key *GetKey();
	bool GetShape(std::vector<float> &shape) const;

	void ForceUpdate()
	{
		m_lastShapeUpdate = -1.0;
	}

protected:
	bool m_useShapeDrivers;
	double m_lastShapeUpdate;
	Key *m_key;
};

#endif

