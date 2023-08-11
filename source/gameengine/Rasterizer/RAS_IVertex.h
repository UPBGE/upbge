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

/** \file RAS_IVertex.h
 *  \ingroup bgerast
 */

#pragma once

#include "BLI_math_vector.h"

#include "MT_Matrix4x4.h"
#include "MT_Vector2.h"
#include "MT_Vector3.h"

/// Struct used to pass the vertex format to functions.
struct RAS_VertexFormat {
  unsigned int uvSize;
  unsigned int colorSize;
};

/// Operators used to compare the contents (uv size, color size, ...) of two vertex formats.
bool operator==(const RAS_VertexFormat &format1, const RAS_VertexFormat &format2);
bool operator!=(const RAS_VertexFormat &format1, const RAS_VertexFormat &format2);

class RAS_VertexInfo {
 public:
  enum {
    FLAT = 1,
  };

 private:
  unsigned int m_origindex;  // 4
  short m_softBodyIndex;     // 2
  short m_flag;              // 2

 public:
  RAS_VertexInfo(unsigned int origindex, bool flat);
  ~RAS_VertexInfo();

  inline const unsigned int getOrigIndex() const
  {
    return m_origindex;
  }

  inline short int getSoftBodyIndex() const
  {
    return m_softBodyIndex;
  }

  inline void setSoftBodyIndex(short int sbIndex)
  {
    m_softBodyIndex = sbIndex;
  }

  inline const short getFlag() const
  {
    return m_flag;
  }

  inline void SetFlag(const short flag)
  {
    m_flag = flag;
  }
};

class RAS_IVertex {
 public:
  enum { MAX_UNIT = 8 };

 protected:
  float m_tangent[4];   // 4*4 = 16
  float m_localxyz[3];  // 3 * 4 = 12
  float m_normal[3];    // 3*4 = 12

 public:
  RAS_IVertex()
  {
  }
  RAS_IVertex(const MT_Vector3 &xyz, const MT_Vector4 &tangent, const MT_Vector3 &normal);

  virtual ~RAS_IVertex();

  virtual const unsigned short getUvSize() const = 0;
  virtual const float *getUV(const int unit) const = 0;

  virtual void SetUV(const int index, const MT_Vector2 &uv) = 0;
  virtual void SetUV(const int index, const float uv[2]) = 0;

  virtual const unsigned short getColorSize() const = 0;
  virtual const unsigned char *getRGBA(const int index) const = 0;
  virtual const unsigned int getRawRGBA(const int index) const = 0;

  virtual void SetRGBA(const int index, const unsigned int rgba) = 0;
  virtual void SetRGBA(const int index, const MT_Vector4 &rgba) = 0;

  inline const float *getXYZ() const
  {
    return m_localxyz;
  }

  inline const float *getNormal() const
  {
    return m_normal;
  }

  inline const float *getTangent() const
  {
    return m_tangent;
  }

  inline MT_Vector3 xyz() const
  {
    return MT_Vector3(m_localxyz);
  }

  inline void SetXYZ(const MT_Vector3 &xyz)
  {
    xyz.getValue(m_localxyz);
  }

  inline void SetXYZ(const float xyz[3])
  {
    copy_v3_v3(m_localxyz, xyz);
  }

  inline void SetNormal(const MT_Vector3 &normal)
  {
    normal.getValue(m_normal);
  }

  inline void SetTangent(const MT_Vector4 &tangent)
  {
    tangent.getValue(m_tangent);
  }

  // compare two vertices, to test if they can be shared, used for
  // splitting up based on uv's, colors, etc
  inline const bool closeTo(const RAS_IVertex *other)
  {
    BLI_assert(getUvSize() == other->getUvSize());
    BLI_assert(getColorSize() == other->getColorSize());
    static const float eps = FLT_EPSILON;
    for (int i = 0, size = getUvSize(); i < size; ++i) {
      if (!compare_v2v2(getUV(i), other->getUV(i), eps)) {
        return false;
      }
    }

    for (int i = 0, size = getColorSize(); i < size; ++i) {
      if (getRawRGBA(i) != other->getRawRGBA(i)) {
        return false;
      }
    }

    return (/* m_flag == other->m_flag && */
            /* at the moment the face only stores the smooth/flat setting so don't bother comparing
               it */
            compare_v3v3(m_normal, other->m_normal, eps) &&
            compare_v3v3(m_tangent, other->m_tangent, eps)
            /* don't bother comparing m_localxyz since we know there from the same vert */
            /* && compare_v3v3(m_localxyz, other->m_localxyz, eps))*/
    );
  }

  inline void Transform(const MT_Matrix4x4 &mat, const MT_Matrix4x4 &nmat)
  {
    SetXYZ((mat * MT_Vector4(m_localxyz[0], m_localxyz[1], m_localxyz[2], 1.0f)).to3d());
    SetNormal((nmat * MT_Vector4(m_normal[0], m_normal[1], m_normal[2], 1.0f)).to3d());
    SetTangent((nmat * MT_Vector4(m_tangent[0], m_tangent[1], m_tangent[2], 1.0f)));
  }

  inline void TransformUV(const int index, const MT_Matrix4x4 &mat)
  {
    SetUV(index, (mat * MT_Vector4(getUV(index)[0], getUV(index)[1], 0.0f, 1.0f)).to2d());
  }
};
