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

/** \file RAS_Vertex.h
 *  \ingroup bgerast
 */

#pragma once

#include "RAS_IVertex.h"

template<class Vertex> class RAS_DisplayArray;

template<unsigned int uvSize, unsigned int colorSize> class RAS_Vertex : public RAS_IVertex {
  friend class RAS_DisplayArray<RAS_Vertex<uvSize, colorSize>>;

 public:
  enum { UvSize = uvSize, ColorSize = colorSize };

 private:
  float m_uvs[UvSize][2];
  unsigned int m_rgba[ColorSize];

 public:
  RAS_Vertex()
  {
  }

  RAS_Vertex(const MT_Vector3 &xyz,
             const MT_Vector2 uvs[UvSize],
             const MT_Vector4 &tangent,
             const unsigned int rgba[ColorSize],
             const MT_Vector3 &normal)
      : RAS_IVertex(xyz, tangent, normal)
  {
    for (int i = 0; i < UvSize; ++i) {
      uvs[i].getValue(m_uvs[i]);
    }

    for (unsigned short i = 0; i < ColorSize; ++i) {
      m_rgba[i] = rgba[i];
    }
  }

  virtual ~RAS_Vertex()
  {
  }

  virtual const unsigned short getUvSize() const
  {
    return UvSize;
  }

  virtual const float *getUV(const int unit) const
  {
    return m_uvs[unit];
  }

  virtual void SetUV(const int index, const MT_Vector2 &uv)
  {
    uv.getValue(m_uvs[index]);
  }

  virtual void SetUV(const int index, const float uv[2])
  {
    copy_v2_v2(m_uvs[index], uv);
  }

  virtual const unsigned short getColorSize() const
  {
    return ColorSize;
  }

  virtual const unsigned char *getRGBA(const int index) const
  {
    return (unsigned char *)&m_rgba[index];
  }

  virtual const unsigned int getRawRGBA(const int index) const
  {
    return m_rgba[index];
  }

  virtual void SetRGBA(const int index, const unsigned int rgba)
  {
    m_rgba[index] = rgba;
  }

  virtual void SetRGBA(const int index, const MT_Vector4 &rgba)
  {
    unsigned char *colp = (unsigned char *)&m_rgba[index];
    colp[0] = (unsigned char)(rgba[0] * 255.0f);
    colp[1] = (unsigned char)(rgba[1] * 255.0f);
    colp[2] = (unsigned char)(rgba[2] * 255.0f);
    colp[3] = (unsigned char)(rgba[3] * 255.0f);
  }
};
