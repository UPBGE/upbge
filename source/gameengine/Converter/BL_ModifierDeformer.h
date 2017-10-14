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

/** \file BL_ModifierDeformer.h
 *  \ingroup bgeconv
 */

#ifndef __BL_MODIFIERDEFORMER_H__
#define __BL_MODIFIERDEFORMER_H__

#ifdef _MSC_VER
#  pragma warning (disable:4786)  /* get rid of stupid stl-visual compiler debug warning */
#endif

#include "BL_ShapeDeformer.h"

class RAS_Mesh;
struct DerivedMesh;
struct Object;

class BL_ModifierDeformer : public BL_ShapeDeformer
{
public:
	static bool HasCompatibleDeformer(Object *ob);
	static bool HasArmatureDeformer(Object *ob);

	BL_ModifierDeformer(KX_GameObject *gameobj,
						Scene *scene,
						Object *bmeshobj_old,
						Object *bmeshobj_new,
						RAS_Mesh *mesh,
						BL_ArmatureObject *arma)
		:BL_ShapeDeformer(gameobj, bmeshobj_old, bmeshobj_new, mesh, arma),
		m_lastModifierUpdate(-1),
		m_scene(scene),
		m_dm(nullptr)
	{
	}

	virtual ~BL_ModifierDeformer();

	virtual void Update(unsigned short reason);
	virtual unsigned short NeedUpdate() const;
	void ForceUpdate()
	{
		m_lastModifierUpdate = -1.0;
	}

protected:
	void UpdateBounds();
	virtual void UpdateTransverts();

	double m_lastModifierUpdate;
	Scene *m_scene;
	DerivedMesh *m_dm;
};

#endif  /* __BL_MODIFIERDEFORMER_H__ */
