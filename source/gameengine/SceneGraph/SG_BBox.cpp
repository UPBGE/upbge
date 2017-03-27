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
 * Bounding Box
 */

/** \file gameengine/SceneGraph/SG_BBox.cpp
 *  \ingroup bgesg
 */

#include "SG_BBox.h"

SG_BBox::SG_BBox()
{
	Set(MT_Vector3(0.0f, 0.0f, 0.0f), MT_Vector3(0.0f, 0.0f, 0.0f));
}

SG_BBox::SG_BBox(const MT_Vector3 &min, const MT_Vector3 &max)
{
	Set(min, max);
}

void SG_BBox::UpdateSphere()
{
	m_center = (m_max + m_min) * 0.5f;
	m_radius = m_center.distance(m_min);
}

const MT_Vector3& SG_BBox::GetCenter() const
{
	return m_center;
}

const float SG_BBox::GetRadius() const
{
	return m_radius;
}

const MT_Vector3& SG_BBox::GetMin() const
{
	return m_min;
}

const MT_Vector3& SG_BBox::GetMax() const
{
	return m_max;
}

void SG_BBox::Get(MT_Vector3& min, MT_Vector3& max) const
{
	min = m_min;
	max = m_max;
}

void SG_BBox::SetMin(const MT_Vector3& min)
{
	m_min = min;
	UpdateSphere();
}

void SG_BBox::SetMax(const MT_Vector3& max)
{
	m_max = max;
	UpdateSphere();
}

void SG_BBox::Set(const MT_Vector3& min, const MT_Vector3& max)
{
	m_min = min;
	m_max = max;
	UpdateSphere();
}

bool SG_BBox::Inside(const MT_Vector3& point) const
{
	return point[0] >= m_min[0] && point[0] <= m_max[0] &&
	       point[1] >= m_min[1] && point[1] <= m_max[1] &&
	       point[2] >= m_min[2] && point[2] <= m_max[2];
}
