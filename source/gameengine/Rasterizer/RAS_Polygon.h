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

/** \file RAS_Polygon.h
 *  \ingroup bgerast
 */

#pragma once

class RAS_IDisplayArray;
class RAS_MaterialBucket;
class RAS_IVertex;
class RAS_VertexInfo;

class RAS_Polygon {
  // location
  RAS_MaterialBucket *m_bucket;
  RAS_IDisplayArray *m_darray;
  unsigned int m_offset[4];
  unsigned short m_numvert;
  unsigned short m_polyflags;

 public:
  enum { VISIBLE = 1, COLLIDER = 2, TWOSIDE = 4 };

  RAS_Polygon(RAS_MaterialBucket *bucket, RAS_IDisplayArray *darray, int numvert);
  virtual ~RAS_Polygon()
  {
  }

  int VertexCount() const;
  RAS_IVertex *GetVertex(int i) const;
  const RAS_VertexInfo &GetVertexInfo(unsigned int i) const;

  void SetVertexOffset(int i, unsigned int offset);
  unsigned int GetVertexOffset(unsigned int i) const;

  bool IsVisible() const;
  void SetVisible(bool visible);

  bool IsCollider() const;
  void SetCollider(bool collider);

  bool IsTwoside() const;
  void SetTwoside(bool twoside);

  RAS_MaterialBucket *GetMaterial() const;
  RAS_IDisplayArray *GetDisplayArray() const;
};
