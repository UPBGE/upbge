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

/** \file gameengine/Rasterizer/RAS_IVertex.cpp
 *  \ingroup bgerast
 */

#include "RAS_IVertex.h"

bool operator==(const RAS_VertexFormat &format1, const RAS_VertexFormat &format2)
{
  return (format1.uvSize == format2.uvSize && format1.colorSize == format2.colorSize);
}

bool operator!=(const RAS_VertexFormat &format1, const RAS_VertexFormat &format2)
{
  return !(format1 == format2);
}

RAS_VertexInfo::RAS_VertexInfo(unsigned int origindex, bool flat)
    : m_origindex(origindex), m_softBodyIndex(-1)
{
  m_flag = (flat) ? FLAT : 0;
}

RAS_VertexInfo::~RAS_VertexInfo()
{
}

RAS_IVertex::RAS_IVertex(const MT_Vector3 &xyz,
                         const MT_Vector4 &tangent,
                         const MT_Vector3 &normal)
{
  xyz.getValue(m_localxyz);
  SetNormal(normal);
  SetTangent(tangent);
}

RAS_IVertex::~RAS_IVertex()
{
}
