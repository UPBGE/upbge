/*
  Vectorial
  Copyright (c) 2010 Mikko Lehtonen
  Copyright (c) 2014 Google, Inc.
  Licensed under the terms of the two-clause BSD License (see LICENSE)
*/
#ifndef VECTORIAL_SIMD4F_SSE_H
#define VECTORIAL_SIMD4F_SSE_H

// Conditionally enable SSE4.1 otherwise fallback to SSE.
#if defined(_M_IX86_FP)
    #if _M_IX86_FP >=2
        #define VECTORIAL_USE_SSE4_1
    #endif
#elif defined(__SSE4_1__)
        #define VECTORIAL_USE_SSE4_1
#endif

#include <xmmintrin.h>
#if defined(VECTORIAL_USE_SSE4_1)
    #include <smmintrin.h>
#endif
#include <string.h>  // memcpy

#ifdef __cplusplus
extern "C" {
#endif


typedef __m128 simd4f; 

typedef union {
    simd4f s ;
    float f[4];
    unsigned int ui[4];
} _simd4f_union;

// creating

vectorial_inline simd4f simd4f_create(float x, float y, float z, float w) {
    simd4f s = { x, y, z, w };
    return s;
}

vectorial_inline simd4f simd4f_zero() { return _mm_setzero_ps(); }

vectorial_inline simd4f simd4f_uload4(const float *ary) {
    simd4f s = _mm_loadu_ps(ary);
    return s;
}

vectorial_inline simd4f simd4f_uload3(const float *ary) {
    simd4f s = simd4f_create(ary[0], ary[1], ary[2], 0);
    return s;
}

vectorial_inline simd4f simd4f_uload2(const float *ary) {
    simd4f s = simd4f_create(ary[0], ary[1], 0, 0);
    return s;
}


vectorial_inline void simd4f_ustore4(const simd4f val, float *ary) {
    _mm_storeu_ps(ary, val);
}

vectorial_inline void simd4f_ustore3(const simd4f val, float *ary) {
    memcpy(ary, &val, sizeof(float) * 3);
}

vectorial_inline void simd4f_ustore2(const simd4f val, float *ary) {
    memcpy(ary, &val, sizeof(float) * 2);
}


// utilites

vectorial_inline simd4f simd4f_splat(float v) { 
    simd4f s = _mm_set1_ps(v); 
    return s;
}

vectorial_inline simd4f simd4f_splat_x(simd4f v) { 
    simd4f s = _mm_shuffle_ps(v, v, _MM_SHUFFLE(0,0,0,0)); 
    return s;
}

vectorial_inline simd4f simd4f_splat_y(simd4f v) { 
    simd4f s = _mm_shuffle_ps(v, v, _MM_SHUFFLE(1,1,1,1)); 
    return s;
}

vectorial_inline simd4f simd4f_splat_z(simd4f v) { 
    simd4f s = _mm_shuffle_ps(v, v, _MM_SHUFFLE(2,2,2,2)); 
    return s;
}

vectorial_inline simd4f simd4f_splat_w(simd4f v) { 
    simd4f s = _mm_shuffle_ps(v, v, _MM_SHUFFLE(3,3,3,3)); 
    return s;
}


// arithmetic

vectorial_inline simd4f simd4f_add(simd4f lhs, simd4f rhs) {
    simd4f ret = _mm_add_ps(lhs, rhs);
    return ret;
}

vectorial_inline simd4f simd4f_sub(simd4f lhs, simd4f rhs) {
    simd4f ret = _mm_sub_ps(lhs, rhs);
    return ret;
}

vectorial_inline simd4f simd4f_mul(simd4f lhs, simd4f rhs) {
    simd4f ret = _mm_mul_ps(lhs, rhs);
    return ret;
}

vectorial_inline simd4f simd4f_div(simd4f lhs, simd4f rhs) {
    simd4f ret = _mm_div_ps(lhs, rhs);
    return ret;
}

vectorial_inline simd4f simd4f_madd(simd4f m1, simd4f m2, simd4f a) {
    return simd4f_add( simd4f_mul(m1, m2), a );
}




vectorial_inline simd4f simd4f_reciprocal(simd4f v) { 
    simd4f s = _mm_rcp_ps(v); 
    const simd4f two = simd4f_create(2.0f, 2.0f, 2.0f, 2.0f);
    s = simd4f_mul(s, simd4f_sub(two, simd4f_mul(v, s)));
    return s;
}

vectorial_inline simd4f simd4f_sqrt(simd4f v) { 
    simd4f s = _mm_sqrt_ps(v); 
    return s;
}

vectorial_inline simd4f simd4f_rsqrt(simd4f v) { 
    simd4f s = _mm_rsqrt_ps(v); 
    const simd4f half = simd4f_create(0.5f, 0.5f, 0.5f, 0.5f);
    const simd4f three = simd4f_create(3.0f, 3.0f, 3.0f, 3.0f);
    s = simd4f_mul(simd4f_mul(s, half), simd4f_sub(three, simd4f_mul(s, simd4f_mul(v,s))));
    return s;
}

vectorial_inline float simd4f_get_x(simd4f s) { _simd4f_union u={s}; return u.f[0]; }
vectorial_inline float simd4f_get_y(simd4f s) { _simd4f_union u={s}; return u.f[1]; }
vectorial_inline float simd4f_get_z(simd4f s) { _simd4f_union u={s}; return u.f[2]; }
vectorial_inline float simd4f_get_w(simd4f s) { _simd4f_union u={s}; return u.f[3]; }

vectorial_inline simd4f simd4f_dot3(simd4f lhs,simd4f rhs) {
#if defined(VECTORIAL_USE_SSE4_1)
    return _mm_dp_ps(lhs, rhs, 0x7f);
#else
    simd4f_aligned16 const unsigned int mask_array[] = { 0xffffffff, 0xffffffff, 0xffffffff, 0 };
    const simd4f mask = _mm_load_ps((const float*)mask_array);
    const simd4f m = _mm_mul_ps(lhs, rhs);
    const simd4f s0 = _mm_and_ps(m, mask);
    const simd4f s1 = _mm_add_ps(s0, _mm_movehl_ps(s0, s0));
    const simd4f s2 = _mm_add_ss(s1, _mm_shuffle_ps(s1, s1, 1));
    return _mm_shuffle_ps(s2,s2, 0);
#endif
}

vectorial_inline float simd4f_dot3_scalar(simd4f lhs,simd4f rhs) {
    return simd4f_get_x(simd4f_dot3(lhs, rhs));
}

vectorial_inline simd4f simd4f_cross3(simd4f lhs, simd4f rhs) {
    
    const simd4f lyzx = _mm_shuffle_ps(lhs, lhs, _MM_SHUFFLE(3,0,2,1));
    const simd4f lzxy = _mm_shuffle_ps(lhs, lhs, _MM_SHUFFLE(3,1,0,2));

    const simd4f ryzx = _mm_shuffle_ps(rhs, rhs, _MM_SHUFFLE(3,0,2,1));
    const simd4f rzxy = _mm_shuffle_ps(rhs, rhs, _MM_SHUFFLE(3,1,0,2));

    return _mm_sub_ps(_mm_mul_ps(lyzx, rzxy), _mm_mul_ps(lzxy, ryzx));

}

vectorial_inline simd4f simd4f_shuffle_wxyz(simd4f s) { return _mm_shuffle_ps(s,s, _MM_SHUFFLE(2,1,0,3) ); }
vectorial_inline simd4f simd4f_shuffle_zwxy(simd4f s) { return _mm_shuffle_ps(s,s, _MM_SHUFFLE(1,0,3,2) ); }
vectorial_inline simd4f simd4f_shuffle_yzwx(simd4f s) { return _mm_shuffle_ps(s,s, _MM_SHUFFLE(0,3,2,1) ); }

vectorial_inline simd4f simd4f_zero_w(simd4f s) {
    simd4f r = _mm_unpackhi_ps(s, _mm_setzero_ps());
    return _mm_movelh_ps(s, r);
}

vectorial_inline simd4f simd4f_zero_zw(simd4f s) {
    return _mm_movelh_ps(s, _mm_setzero_ps());
}

vectorial_inline simd4f simd4f_merge_high(simd4f xyzw, simd4f abcd) { 
    return _mm_movehl_ps(abcd, xyzw);
}


typedef simd4f_aligned16 union {
    unsigned int ui[4];
    float f[4];
} _simd4f_uif;

vectorial_inline simd4f simd4f_flip_sign_0101(simd4f s) {
    const _simd4f_uif upnpn = { { 0x00000000, 0x80000000, 0x00000000, 0x80000000 } };
    return _mm_xor_ps( s, _mm_load_ps(upnpn.f) ); 
}

vectorial_inline simd4f simd4f_flip_sign_1010(simd4f s) {
    const _simd4f_uif unpnp = { { 0x80000000, 0x00000000, 0x80000000, 0x00000000 } };
    return _mm_xor_ps( s, _mm_load_ps(unpnp.f) ); 
}

vectorial_inline simd4f simd4f_min(simd4f a, simd4f b) {
    return _mm_min_ps( a, b ); 
}

vectorial_inline simd4f simd4f_max(simd4f a, simd4f b) {
    return _mm_max_ps( a, b ); 
}



#ifdef __cplusplus
}
#endif


#endif
