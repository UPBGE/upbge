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

/** \file moto/include/MT_Vector2.h
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

#ifndef MT_VECTOR2_H
#define MT_VECTOR2_H

#include "MT_Config.h"

#include <BLI_utildefines.h>
#include "MT_Scalar.h"
#include "MT_Stream.h"

class MT_Vector2 
{
public:
    explicit MT_Vector2() {}
    template <typename T>
    explicit MT_Vector2(const T *vv) { setValue(vv); }
    explicit MT_Vector2(MT_Scalar xx, MT_Scalar yy) { setValue(xx, yy); }
    
    MT_Scalar&       operator[](int i)       { return m_co[i]; }
    const MT_Scalar& operator[](int i) const { return m_co[i]; }
    
    MT_Scalar&       x()       { return m_co[0]; } 
    const MT_Scalar& x() const { return m_co[0]; } 

    MT_Scalar&       y()       { return m_co[1]; }
    const MT_Scalar& y() const { return m_co[1]; } 

	MT_Scalar&       u()       { return m_co[0]; } 
    const MT_Scalar& u() const { return m_co[0]; } 

    MT_Scalar&       v()       { return m_co[1]; }
    const MT_Scalar& v() const { return m_co[1]; } 

    MT_Scalar       *getValue()       { return m_co; }
    const MT_Scalar *getValue() const { return m_co; }

    template <typename T>
    void getValue(T *vv) const { 
        vv[0] = (T)m_co[0]; vv[1] = (T)m_co[1];
    }
    
    template <typename T>
    void setValue(const T *vv) {
        m_co[0] = (MT_Scalar)vv[0]; m_co[1] = (MT_Scalar)vv[1];
    }
    
    void setValue(MT_Scalar xx, MT_Scalar yy) {
        m_co[0] = xx; m_co[1] = yy; 
    }

    MT_Scalar  distance(const MT_Vector2& p) const;
    MT_Scalar  distance2(const MT_Vector2& p) const;

    MT_Vector2  lerp(const MT_Vector2& p, MT_Scalar t) const;

    MT_Vector2& operator=(const MT_Vector2& v);
    MT_Vector2& operator+=(const MT_Vector2& v);
    MT_Vector2& operator-=(const MT_Vector2& v);
    MT_Vector2& operator*=(MT_Scalar s);
    MT_Vector2& operator/=(MT_Scalar s);
  
    MT_Scalar   dot(const MT_Vector2& v) const; 

    MT_Scalar   length2() const;
    MT_Scalar   length() const;

    MT_Vector2  absolute() const;

    void        normalize();
    MT_Vector2  normalized() const;

    void        scale(MT_Scalar x, MT_Scalar y); 
    MT_Vector2  scaled(MT_Scalar x, MT_Scalar y) const; 
    
    bool        fuzzyZero() const; 

    MT_Scalar   angle(const MT_Vector2& v) const;
    MT_Vector2  cross(const MT_Vector2& v) const;
    MT_Scalar   triple(const MT_Vector2& v1, const MT_Vector2& v2) const;

    int         closestAxis() const;

    static MT_Vector2 random();
    
protected:
    MT_Scalar m_co[2];                            
};

inline bool operator==(const MT_Vector2& t1, const MT_Vector2& t2) {
    return t1[0] == t2[0] && t1[1] == t2[1];
}

inline MT_OStream& operator<<(MT_OStream& os, const MT_Vector2& t) {
    return os << t[0] << ' ' << t[1];
}

MT_Scalar MT_distance(const MT_Vector2& p1, const MT_Vector2& p2);
MT_Scalar MT_distance2(const MT_Vector2& p1, const MT_Vector2& p2);

MT_Vector2 MT_lerp(const MT_Vector2& p1, const MT_Vector2& p2, MT_Scalar t);

MT_Vector2 operator+(const MT_Vector2& v1, const MT_Vector2& v2);
MT_Vector2 operator-(const MT_Vector2& v1, const MT_Vector2& v2);
MT_Vector2 operator-(const MT_Vector2& v);
MT_Vector2 operator*(const MT_Vector2& v, MT_Scalar s);
MT_Vector2 operator*(MT_Scalar s, const MT_Vector2& v);
MT_Vector2 operator/(const MT_Vector2& v, MT_Scalar s);

MT_Scalar  MT_dot(const MT_Vector2& v1, const MT_Vector2& v2);

MT_Scalar  MT_length2(const MT_Vector2& v);
MT_Scalar  MT_length(const MT_Vector2& v);

bool       MT_fuzzyZero(const MT_Vector2& v);
bool       MT_fuzzyEqual(const MT_Vector2& v1, const MT_Vector2& v2);

MT_Scalar  MT_angle(const MT_Vector2& v1, const MT_Vector2& v2);
MT_Vector2 MT_cross(const MT_Vector2& v1, const MT_Vector2& v2);
MT_Scalar  MT_triple(const MT_Vector2& v1, const MT_Vector2& v2, 
                     const MT_Vector2& v3);

#ifdef GEN_INLINED
#include "MT_Vector2.inl"
#endif

#endif

