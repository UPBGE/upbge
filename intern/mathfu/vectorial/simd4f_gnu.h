/*
  Vectorial
  Copyright (c) 2010 Mikko Lehtonen
  Licensed under the terms of the two-clause BSD License (see LICENSE)
*/
#ifndef VECTORIAL_SIMD4F_GNU_H
#define VECTORIAL_SIMD4F_GNU_H

#include <math.h>
#include <string.h>  // memcpy


#ifdef __cplusplus
extern "C" {
#endif


typedef float simd4f __attribute__ ((vector_size (16)));

typedef union {
    simd4f s ;
    float f[4];
} _simd4f_union;

vectorial_inline float simd4f_get_x(simd4f s) { _simd4f_union u={s}; return u.f[0]; }
vectorial_inline float simd4f_get_y(simd4f s) { _simd4f_union u={s}; return u.f[1]; }
vectorial_inline float simd4f_get_z(simd4f s) { _simd4f_union u={s}; return u.f[2]; }
vectorial_inline float simd4f_get_w(simd4f s) { _simd4f_union u={s}; return u.f[3]; }


vectorial_inline simd4f simd4f_create(float x, float y, float z, float w) {
    simd4f s = { x, y, z, w };
    return s;
}

vectorial_inline simd4f simd4f_zero() { return simd4f_create(0.0f, 0.0f, 0.0f, 0.0f); }

vectorial_inline simd4f simd4f_uload4(const float *ary) {
    simd4f s = { ary[0], ary[1], ary[2], ary[3] };
    return s;
}

vectorial_inline simd4f simd4f_uload3(const float *ary) {
    simd4f s = { ary[0], ary[1], ary[2], 0 };
    return s;
}

vectorial_inline simd4f simd4f_uload2(const float *ary) {
    simd4f s = { ary[0], ary[1], 0, 0 };
    return s;
}


vectorial_inline void simd4f_ustore4(const simd4f val, float *ary) {
    memcpy(ary, &val, sizeof(float) * 4);
}

vectorial_inline void simd4f_ustore3(const simd4f val, float *ary) {
    memcpy(ary, &val, sizeof(float) * 3);
}

vectorial_inline void simd4f_ustore2(const simd4f val, float *ary) {
    memcpy(ary, &val, sizeof(float) * 2);
}


vectorial_inline simd4f simd4f_splat(float v) { 
    simd4f s = { v, v, v, v }; 
    return s;
}

vectorial_inline simd4f simd4f_splat_x(simd4f v) { 
    float s = simd4f_get_x(v);
    simd4f ret = { s, s, s, s }; 
    return ret;
}

vectorial_inline simd4f simd4f_splat_y(simd4f v) { 
    float s = simd4f_get_y(v);
    simd4f ret = { s, s, s, s }; 
    return ret;
}

vectorial_inline simd4f simd4f_splat_z(simd4f v) { 
    float s = simd4f_get_z(v);
    simd4f ret = { s, s, s, s }; 
    return ret;
}

vectorial_inline simd4f simd4f_splat_w(simd4f v) { 
    float s = simd4f_get_w(v);
    simd4f ret = { s, s, s, s }; 
    return ret;
}

vectorial_inline simd4f simd4f_reciprocal(simd4f v) { 
    return simd4f_splat(1.0f) / v;
}

vectorial_inline simd4f simd4f_sqrt(simd4f v) { 
    simd4f ret = { sqrtf(simd4f_get_x(v)), sqrtf(simd4f_get_y(v)), sqrtf(simd4f_get_z(v)), sqrtf(simd4f_get_w(v)) };
    return ret;
}

vectorial_inline simd4f simd4f_rsqrt(simd4f v) { 
    return simd4f_splat(1.0f) / simd4f_sqrt(v);
}



vectorial_inline simd4f simd4f_add(simd4f lhs, simd4f rhs) {
    simd4f ret = lhs + rhs;
    return ret;
}

vectorial_inline simd4f simd4f_sub(simd4f lhs, simd4f rhs) {
    simd4f ret = lhs - rhs;
    return ret;
}

vectorial_inline simd4f simd4f_mul(simd4f lhs, simd4f rhs) {
    simd4f ret = lhs * rhs;
    return ret;
}

vectorial_inline simd4f simd4f_div(simd4f lhs, simd4f rhs) {
    simd4f ret = lhs / rhs;
    return ret;
}

vectorial_inline simd4f simd4f_madd(simd4f m1, simd4f m2, simd4f a) {
    return simd4f_add( simd4f_mul(m1, m2), a );
}

vectorial_inline float simd4f_dot3_scalar(simd4f lhs, simd4f rhs) {
    _simd4f_union l = {lhs};
    _simd4f_union r = {rhs};
    return l.f[0] * r.f[0] + l.f[1] * r.f[1] + l.f[2] * r.f[2];
}

vectorial_inline simd4f simd4f_dot3(simd4f lhs, simd4f rhs) {
    return simd4f_splat( simd4f_dot3_scalar(lhs, rhs) );
}

vectorial_inline simd4f simd4f_cross3(simd4f l, simd4f r) {
    _simd4f_union lhs = {l};
    _simd4f_union rhs = {r};
    
    return simd4f_create( lhs.f[1] * rhs.f[2] - lhs.f[2] * rhs.f[1],
                          lhs.f[2] * rhs.f[0] - lhs.f[0] * rhs.f[2],
                          lhs.f[0] * rhs.f[1] - lhs.f[1] * rhs.f[0], 0);
}


vectorial_inline simd4f simd4f_shuffle_wxyz(simd4f s) { 
    _simd4f_union u = {s};
    return simd4f_create(u.f[3], u.f[0], u.f[1], u.f[2]); 
}

vectorial_inline simd4f simd4f_shuffle_zwxy(simd4f s) { 
    _simd4f_union u = {s};
    return simd4f_create(u.f[2], u.f[3], u.f[0], u.f[1]); 
}

vectorial_inline simd4f simd4f_shuffle_yzwx(simd4f s) { 
    _simd4f_union u = {s};
    return simd4f_create(u.f[1], u.f[2], u.f[3], u.f[0]); 
}


vectorial_inline simd4f simd4f_zero_w(simd4f s) {
    _simd4f_union u = {s};
    return simd4f_create(u.f[0], u.f[1], u.f[2], 0.0f);
}

vectorial_inline simd4f simd4f_zero_zw(simd4f s) {
    _simd4f_union u = {s};
    return simd4f_create(u.f[0], u.f[1], 0.0f, 0.0f);
}


vectorial_inline simd4f simd4f_merge_high(simd4f abcd, simd4f xyzw) { 
    _simd4f_union u1 = {abcd};
    _simd4f_union u2 = {xyzw};
    return simd4f_create(u1.f[2], u1.f[3], u2.f[2], u2.f[3]);
}

vectorial_inline simd4f simd4f_flip_sign_0101(simd4f s) {
    _simd4f_union u = {s};
    return simd4f_create(u.f[0], -u.f[1], u.f[2], -u.f[3]);
}

vectorial_inline simd4f simd4f_flip_sign_1010(simd4f s) {
    _simd4f_union u = {s};
    return simd4f_create(-u.f[0], u.f[1], -u.f[2], u.f[3]);
}


vectorial_inline simd4f simd4f_min(simd4f a, simd4f b) {
    _simd4f_union ua = {a};
    _simd4f_union ub = {b};
    return simd4f_create( ua.f[0] < ub.f[0] ? ua.f[0] : ub.f[0], 
                          ua.f[1] < ub.f[1] ? ua.f[1] : ub.f[1], 
                          ua.f[2] < ub.f[2] ? ua.f[2] : ub.f[2], 
                          ua.f[3] < ub.f[3] ? ua.f[3] : ub.f[3] );
}

vectorial_inline simd4f simd4f_max(simd4f a, simd4f b) {
    _simd4f_union ua = {a};
    _simd4f_union ub = {b};
    return simd4f_create( ua.f[0] > ub.f[0] ? ua.f[0] : ub.f[0], 
                          ua.f[1] > ub.f[1] ? ua.f[1] : ub.f[1], 
                          ua.f[2] > ub.f[2] ? ua.f[2] : ub.f[2], 
                          ua.f[3] > ub.f[3] ? ua.f[3] : ub.f[3] );
}



#ifdef __cplusplus
}
#endif


#endif

