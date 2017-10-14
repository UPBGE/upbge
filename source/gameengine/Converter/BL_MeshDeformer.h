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

/** \file BL_MeshDeformer.h
 *  \ingroup bgeconv
 */

#ifndef __BL_MESHDEFORMER_H__
#define __BL_MESHDEFORMER_H__

#include "RAS_Deformer.h"
#include "DNA_object_types.h"
#include "DNA_key_types.h"

#ifdef _MSC_VER
#  pragma warning (disable:4786)  /* get rid of stupid stl-visual compiler debug warning */
#endif

struct Object;
struct Mesh;
class KX_GameObject;
class RAS_Mesh;
class RAS_IPolyMaterial;

class BL_MeshDeformer : public RAS_Deformer
{
public:
	enum UpdateReason {
		UPDATE_DISPLAY_ARRAY = (1 << 0),
		UPDATE_SKIN = (1 << 1),
		UPDATE_SHAPE = (1 << 2),
		UPDATE_MODIFIER = (1 << 3)
	};

	void VerifyStorage();
	void RecalcNormals();

	BL_MeshDeformer(KX_GameObject *gameobj, Object *obj, RAS_Mesh *meshobj);
	virtual ~BL_MeshDeformer();

	virtual unsigned short NeedUpdate() const;
	virtual void Update(unsigned short reason);

	Mesh *GetMesh()
	{
		return m_bmesh;
	}

	void SetLastFrame(double lastFrame);

protected:
	Mesh *m_bmesh;

	// this is so m_transverts doesn't need to be converted
	// before deformation
	std::vector<mt::vec3_packed> m_transverts;
	std::vector<mt::vec3_packed> m_transnors;
	Object *m_objMesh;

	KX_GameObject *m_gameobj;
	/// Last update frame.
	double m_lastDeformUpdate;
	/// Last action update frame.
	double m_lastFrame;
};

#endif

