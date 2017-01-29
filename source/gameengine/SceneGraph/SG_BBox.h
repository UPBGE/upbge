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

/** \file SG_BBox.h
 *  \ingroup bgesg
 *  \brief Bounding Box
 */

#ifndef __SG_BBOX_H__
#define __SG_BBOX_H__

#include "MT_Scalar.h"
#include "MT_Vector3.h"
#include "MT_Vector3.h"
#include "MT_Transform.h"

#include <vector>

class SG_Node;

/**
 * Bounding box class.
 * Holds the minimum and maximum axis aligned points of a node's bounding box,
 * in world coordinates.
 */
class SG_BBox
{
	MT_Vector3 m_min;
	MT_Vector3 m_max;

public:
	typedef enum {
		INSIDE,
		INTERSECT,
		OUTSIDE
	} intersect;

	SG_BBox();
	SG_BBox(const MT_Vector3 &min, const MT_Vector3 &max);
	SG_BBox(const SG_BBox &other, const MT_Transform &world);
	SG_BBox(const SG_BBox &other);
	~SG_BBox();

	/**
	 * Enlarges the bounding box to contain the specified point.
	 */
	SG_BBox& operator +=(const MT_Vector3 &point);
	/**
	 * Enlarges the bounding box to contain the specified bound box.
	 */
	SG_BBox& operator +=(const SG_BBox &bbox);

	SG_BBox operator +(const SG_BBox &bbox2) const;

	SG_BBox transform(const MT_Transform &world) const;
	/**
	 * Computes the volume of the bounding box.
	 */
	MT_Scalar volume() const;

	/**
	 * Test if the given point is inside this bounding box.
	 */
	bool inside(const MT_Vector3 &point) const;

	/**
	 * Test if the given bounding box is inside this bounding box.
	 */
	bool inside(const SG_BBox &other) const;

	/**
	 * Test if the given bounding box is outside this bounding box.
	 */
	bool outside(const SG_BBox &other) const;

	/**
	 * Test if the given bounding box intersects this bounding box.
	 */
	bool intersects(const SG_BBox &other) const;

	/**
	 * Test the given bounding box with this bounding box.
	 */
	intersect test(const SG_BBox &other) const;

	/**
	 * Get the eight points that define this bounding box.
	 *
	 * \param world a world transform to apply to the produced points bounding box.
	 */
	void get(MT_Vector3 *box, const MT_Transform &world) const;
	/**
	 * Get the eight points that define this axis aligned bounding box.
	 * This differs from SG_BBox::get() in that the produced box will be world axis aligned.
	 * The maximum & minimum local points will be transformed *before* splitting to 8 points.
	 * \param world a world transform to be applied.
	 */
	void getaa(MT_Vector3 *box, const MT_Transform &world) const;

	void getmm(MT_Vector3 *box, const MT_Transform &world) const;

	void split(SG_BBox &left, SG_BBox &right) const;

	const MT_Vector3 GetCenter() const;
	const MT_Scalar GetRadius() const;

	const MT_Vector3& GetMin() const;
	const MT_Vector3& GetMax() const;

	void SetMin(const MT_Vector3& min);
	void SetMax(const MT_Vector3& max);
};

#endif  // __SG_BBOX_H__
