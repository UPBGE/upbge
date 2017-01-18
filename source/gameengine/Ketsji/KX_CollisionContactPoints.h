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
 * Contributor(s): Tristan Porteries.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file KX_CollisionContactPoints.h
 *  \ingroup ketsji
 */

#ifndef __KX_COLLISION_CONTACT_POINTS_H__
#define __KX_COLLISION_CONTACT_POINTS_H__

#include "EXP_Value.h"
#include "EXP_ListWrapper.h"

class PHY_CollData;

class KX_CollisionContactPoint : public CValue
{
	Py_Header
protected:
	/// All infos about contact position, normal, friction ect…
	const PHY_CollData *m_collData;
	const unsigned int m_index;
	const bool m_firstObject;

public:
	KX_CollisionContactPoint(const PHY_CollData *collData, unsigned int index, bool firstObject);
	virtual ~KX_CollisionContactPoint();

	// stuff for cvalue related things
	std::string GetName();

#ifdef WITH_PYTHON

	static PyObject *pyattr_get_local_point_a(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_local_point_b(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_world_point(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_normal(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_combined_friction(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_combined_restitution(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_applied_impulse(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);

#endif  // WITH_PYTHON
};

class KX_CollisionContactPointList
{
private:
	/// The list of contact points for a pair of rigid bodies.
	const PHY_CollData *m_collData;
	/// The object is the first in the pair or the second ?
	bool m_firstObject;

public:
	KX_CollisionContactPointList(const PHY_CollData *collData, bool firstObject);
	virtual ~KX_CollisionContactPointList();

#ifdef WITH_PYTHON
	CListWrapper *GetListWrapper();
#endif  // WITH_PYTHON

	KX_CollisionContactPoint *GetCollisionContactPoint(unsigned int index);
	unsigned int GetNumCollisionContactPoint();
	const PHY_CollData *GetCollData();
	bool GetFirstObject();
};

#endif  // __KX_COLLISION_CONTACT_POINTS_H__
