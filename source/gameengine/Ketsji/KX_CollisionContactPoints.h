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

class PHY_ICollData;

class KX_CollisionContactPoint : public EXP_Value
{
	Py_Header(KX_CollisionContactPoint)
protected:
	/// All infos about contact position, normal, friction ect…
	const PHY_ICollData *m_collData;
	const unsigned int m_index;
	const bool m_firstObject;

public:
	KX_CollisionContactPoint(const PHY_ICollData *collData, unsigned int index, bool firstObject);
	virtual ~KX_CollisionContactPoint();

	// stuff for cvalue related things
	virtual std::string GetName() const;

#ifdef WITH_PYTHON

	mt::vec3 pyattr_get_local_point_a();
	mt::vec3 pyattr_get_local_point_b();
	mt::vec3 pyattr_get_world_point();
	mt::vec3 pyattr_get_normal();
	float pyattr_get_combined_friction();
	float pyattr_get_combined_rolling_friction();
	float pyattr_get_combined_restitution();
	float pyattr_get_applied_impulse();

#endif  // WITH_PYTHON
};

class KX_CollisionContactPointList
#ifdef WITH_PYTHON
	: public EXP_BaseListWrapper
#endif  // WITH_PYTHON
{
private:
	/// The list of contact points for a pair of rigid bodies.
	const PHY_ICollData *m_collData;
	/// The object is the first in the pair or the second ?
	bool m_firstObject;

public:
	KX_CollisionContactPointList(const PHY_ICollData *collData, bool firstObject);
	virtual ~KX_CollisionContactPointList();

	virtual std::string GetName() const;

	KX_CollisionContactPoint *GetCollisionContactPoint(unsigned int index);
	unsigned int GetNumCollisionContactPoint();
	const PHY_ICollData *GetCollData();
	bool GetFirstObject();
};

#endif  // __KX_COLLISION_CONTACT_POINTS_H__
