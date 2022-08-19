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

/** \file moto/include/MT_Vector3.h
 *  \ingroup moto
 */


/*

 * Copyright (c) 2000 Gino van den Bergen <gino@acm.org>
 *
 * Permission to use, copy, modify, distribute and sell this software
 * and its documentation for any purpose is hereby granted without fee,
 * provided that the above copyright notice appear in all copies and
 * that both that copyright notice and this permission notice appear
 * in supporting documentation.  Gino van den Bergen makes no
 * representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 */

#ifndef MT_VECTOR3_H
#define MT_VECTOR3_H

#include "MT_Config.h"

#include <BLI_utildefines.h>
#include "MT_Scalar.h"
#include "MT_Stream.h"
#include "MT_Vector2.h"

class MT_Vector3
{
public:
    explicit MT_Vector3() {}
    template <typename T>
    explicit MT_Vector3(const T *v) { setValue(v); }
    explicit MT_Vector3(MT_Scalar xx, MT_Scalar yy, MT_Scalar zz) { setValue(xx, yy, zz); }
    
    MT_Scalar&       operator[](int i)       { return m_co[i]; }
    const MT_Scalar& operator[](int i) const { return m_co[i]; }
    
    MT_Scalar&       x()       { return m_co[0]; } 
    const MT_Scalar& x() const { return m_co[0]; } 

    MT_Scalar&       y()       { return m_co[1]; }
    const MT_Scalar& y() const { return m_co[1]; } 

    MT_Scalar&       z()       { return m_co[2]; } 
    const MT_Scalar& z() const { return m_co[2]; } 

    MT_Scalar       *getValue()       { return m_co; }
    const MT_Scalar *getValue() const { return m_co; }

    template <typename T>
    void getValue(T *v) const { 
        v[0] = (T)m_co[0]; 
		v[1] = (T)m_co[1]; 
		v[2] = (T)m_co[2];
    }

    template <typename T>
    void setValue(const T *v) {
        m_co[0] = (MT_Scalar)v[0]; 
        m_co[1] = (MT_Scalar)v[1]; 
        m_co[2] = (MT_Scalar)v[2];
    }
    
    void setValue(MT_Scalar xx, MT_Scalar yy, MT_Scalar zz) {
        m_co[0] = xx; m_co[1] = yy; m_co[2] = zz;
    }


    MT_Scalar  distance(const MT_Vector3& p) const;
    MT_Scalar  distance2(const MT_Vector3& p) const;

    MT_Vector3  lerp(const MT_Vector3& p, MT_Scalar t) const;

    MT_Vector3& operator=(const MT_Vector3& v);
    MT_Vector3& operator+=(const MT_Vector3& v);
    MT_Vector3& operator-=(const MT_Vector3& v);
    MT_Vector3& operator*=(MT_Scalar s);
    MT_Vector3& operator/=(MT_Scalar s);
  
    MT_Scalar   dot(const MT_Vector3& v) const; 

    MT_Scalar   length2() const;
    MT_Scalar   length() const;

    MT_Vector3  absolute() const;

    void        noiseGate(MT_Scalar threshold);

    void        normalize();
    MT_Vector3  normalized() const;
	MT_Vector3  safe_normalized() const;
	MT_Vector3  safe_normalized_vec(MT_Vector3 vecnormalized) const;

    void        scale(MT_Scalar x, MT_Scalar y, MT_Scalar z); 
    MT_Vector3  scaled(MT_Scalar x, MT_Scalar y, MT_Scalar z) const; 
    
    bool        fuzzyZero() const; 

    MT_Scalar   angle(const MT_Vector3& v) const;
    MT_Vector3  cross(const MT_Vector3& v) const;
    MT_Scalar   triple(const MT_Vector3& v1, const MT_Vector3& v2) const;

    int         closestAxis() const;

	MT_Vector2 to2d() const;

    static MT_Vector3 random();

protected:
    MT_Scalar m_co[3];                            
};

inline bool operator==(const MT_Vector3& t1, const MT_Vector3& t2) {
    return t1[0] == t2[0] && t1[1] == t2[1] && t1[2] == t2[2];
}

inline MT_OStream& operator<<(MT_OStream& os, const MT_Vector3& t) {
    return os << t[0] << ' ' << t[1] << ' ' << t[2];
}

MT_Scalar MT_distance(const MT_Vector3& p1, const MT_Vector3& p2);
MT_Scalar MT_distance2(const MT_Vector3& p1, const MT_Vector3& p2);

MT_Vector3 MT_lerp(const MT_Vector3& p1, const MT_Vector3& p2, MT_Scalar t);

MT_Vector3 operator+(const MT_Vector3& v1, const MT_Vector3& v2);
MT_Vector3 operator-(const MT_Vector3& v1, const MT_Vector3& v2);
MT_Vector3 operator-(const MT_Vector3& v);
MT_Vector3 operator*(const MT_Vector3& v, MT_Scalar s);
MT_Vector3 operator*(MT_Scalar s, const MT_Vector3& v);
MT_Vector3 operator/(const MT_Vector3& v, MT_Scalar s);

MT_Vector3 operator*(const MT_Vector3& v1, const MT_Vector3& v2);

MT_Scalar  MT_dot(const MT_Vector3& v1, const MT_Vector3& v2);

MT_Scalar  MT_length2(const MT_Vector3& v);
MT_Scalar  MT_length(const MT_Vector3& v);

bool       MT_fuzzyZero(const MT_Vector3& v);
bool       MT_fuzzyEqual(const MT_Vector3& v1, const MT_Vector3& v2);

MT_Scalar  MT_angle(const MT_Vector3& v1, const MT_Vector3& v2);
MT_Vector3 MT_cross(const MT_Vector3& v1, const MT_Vector3& v2);
MT_Scalar  MT_triple(const MT_Vector3& v1, const MT_Vector3& v2, 
                     const MT_Vector3& v3);


#ifdef GEN_INLINED
#include "MT_Vector3.inl"
#endif

#endif

