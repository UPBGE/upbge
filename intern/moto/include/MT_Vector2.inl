#include "MT_Optimize.h"

GEN_INLINE MT_Vector2& MT_Vector2::operator=(const MT_Vector2& v) {
    m_co[0] = v[0]; m_co[1] = v[1]; 
    return *this;
}

GEN_INLINE MT_Scalar MT_Vector2::distance(const MT_Vector2& p) const {
    return (p - *this).length();
}

GEN_INLINE MT_Scalar MT_Vector2::distance2(const MT_Vector2& p) const {
    return (p - *this).length2();
}

GEN_INLINE MT_Vector2 MT_Vector2::lerp(const MT_Vector2& p, MT_Scalar t) const {
    return MT_Vector2(m_co[0] + (p[0] - m_co[0]) * t,
                     m_co[1] + (p[1] - m_co[1]) * t);
}

GEN_INLINE MT_Scalar MT_distance(const MT_Vector2& p1, const MT_Vector2& p2) { 
    return p1.distance(p2); 
}

GEN_INLINE MT_Scalar MT_distance2(const MT_Vector2& p1, const MT_Vector2& p2) { 
    return p1.distance2(p2); 
}

GEN_INLINE MT_Vector2 MT_lerp(const MT_Vector2& p1, const MT_Vector2& p2, MT_Scalar t) {
    return p1.lerp(p2, t);
}


GEN_INLINE MT_Vector2& MT_Vector2::operator+=(const MT_Vector2& vv) {
    m_co[0] += vv[0]; m_co[1] += vv[1];
    return *this;
}

GEN_INLINE MT_Vector2& MT_Vector2::operator-=(const MT_Vector2& vv) {
    m_co[0] -= vv[0]; m_co[1] -= vv[1];
    return *this;
}
 
GEN_INLINE MT_Vector2& MT_Vector2::operator*=(MT_Scalar s) {
    m_co[0] *= s; m_co[1] *= s;
    return *this;
}

GEN_INLINE MT_Vector2& MT_Vector2::operator/=(MT_Scalar s) {
    BLI_assert(!MT_fuzzyZero(s));
    return *this *= 1.0f / s;
}

GEN_INLINE MT_Vector2 operator+(const MT_Vector2& v1, const MT_Vector2& v2) {
    return MT_Vector2(v1[0] + v2[0], v1[1] + v2[1]);
}

GEN_INLINE MT_Vector2 operator-(const MT_Vector2& v1, const MT_Vector2& v2) {
    return MT_Vector2(v1[0] - v2[0], v1[1] - v2[1]);
}

GEN_INLINE MT_Vector2 operator-(const MT_Vector2& v) {
    return MT_Vector2(-v[0], -v[1]);
}

GEN_INLINE MT_Vector2 operator*(const MT_Vector2& v, MT_Scalar s) {
    return MT_Vector2(v[0] * s, v[1] * s);
}

GEN_INLINE MT_Vector2 operator*(MT_Scalar s, const MT_Vector2& v) { return v * s; }

GEN_INLINE MT_Vector2 operator/(const MT_Vector2& v, MT_Scalar s) {
    BLI_assert(!MT_fuzzyZero(s));
    return v * (1.0f / s);
}

GEN_INLINE MT_Scalar MT_Vector2::dot(const MT_Vector2& vv) const {
    return m_co[0] * vv[0] + m_co[1] * vv[1];
}

GEN_INLINE MT_Scalar MT_Vector2::length2() const { return dot(*this); }
GEN_INLINE MT_Scalar MT_Vector2::length() const { return sqrtf(length2()); }

GEN_INLINE MT_Vector2 MT_Vector2::absolute() const {
    return MT_Vector2(MT_abs(m_co[0]), MT_abs(m_co[1]));
}

GEN_INLINE bool MT_Vector2::fuzzyZero() const { return MT_fuzzyZero2(length2()); }

GEN_INLINE void MT_Vector2::normalize() { *this /= length(); }
GEN_INLINE MT_Vector2 MT_Vector2::normalized() const { return *this / length(); }

GEN_INLINE void MT_Vector2::scale(MT_Scalar xx, MT_Scalar yy) {
    m_co[0] *= xx; m_co[1] *= yy; 
}

GEN_INLINE MT_Vector2 MT_Vector2::scaled(MT_Scalar xx, MT_Scalar yy) const {
    return MT_Vector2(m_co[0] * xx, m_co[1] * yy);
}

GEN_INLINE MT_Scalar MT_Vector2::angle(const MT_Vector2& vv) const {
    MT_Scalar s = sqrtf(length2() * vv.length2());
    BLI_assert(!MT_fuzzyZero(s));
    return acosf(dot(vv) / s);
}


GEN_INLINE MT_Scalar  MT_dot(const MT_Vector2& v1, const MT_Vector2& v2) { 
    return v1.dot(v2);
}

GEN_INLINE MT_Scalar  MT_length2(const MT_Vector2& v) { return v.length2(); }
GEN_INLINE MT_Scalar  MT_length(const MT_Vector2& v) { return v.length(); }

GEN_INLINE bool       MT_fuzzyZero(const MT_Vector2& v) { return v.fuzzyZero(); }
GEN_INLINE bool       MT_fuzzyEqual(const MT_Vector2& v1, const MT_Vector2& v2) { 
    return MT_fuzzyZero(v1 - v2); 
}

GEN_INLINE MT_Scalar  MT_angle(const MT_Vector2& v1, const MT_Vector2& v2) { return v1.angle(v2); }
