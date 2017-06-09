/*
  Vectorial
  Copyright (c) 2010 Mikko Lehtonen
  Copyright (c) 2014 Google, Inc.
  Licensed under the terms of the two-clause BSD License (see LICENSE)
*/
#ifndef VECTORIAL_SIMD2F_NEON_H
#define VECTORIAL_SIMD2F_NEON_H

#include <arm_neon.h>

#ifdef __cplusplus
extern "C" {
#endif


typedef float32x2_t simd2f;

typedef union {
    simd2f s ;
    float f[2];
} _simd2f_union;



vectorial_inline simd2f simd2f_create(float x, float y) {
    const float32_t d[2] = { x,y };
    simd2f s = vld1_f32(d);
    return s;
}

vectorial_inline simd2f simd2f_zero() { return vdup_n_f32(0.0f); }

vectorial_inline simd2f simd2f_uload2(const float *ary) {
    const float32_t* ary32 = (const float32_t*)ary;
    simd2f s = vld1_f32(ary32);
    return s;
}

vectorial_inline void simd2f_ustore2(const simd2f val, float *ary) {
    vst1_f32( (float32_t*)ary, val);
}

vectorial_inline simd2f simd2f_splat(float v) {
    simd2f s = vdup_n_f32(v);
    return s;
}

vectorial_inline simd2f simd2f_splat_x(simd2f v) {
    simd2f ret = vdup_lane_f32(v, 0);
    return ret;
}

vectorial_inline simd2f simd2f_splat_y(simd2f v) {
    simd2f ret = vdup_lane_f32(v, 1);
    return ret;
}

vectorial_inline simd2f simd2f_reciprocal(simd2f v) {
    simd2f estimate = vrecpe_f32(v);
    estimate = vmul_f32(vrecps_f32(estimate, v), estimate);
    estimate = vmul_f32(vrecps_f32(estimate, v), estimate);
    return estimate;
}

vectorial_inline void simd2f_rsqrt_1iteration(const simd2f& v, simd2f& estimate) {
    simd2f estimate2 = vmul_f32(estimate, v);
    estimate = vmul_f32(estimate, vrsqrts_f32(estimate2, estimate));
}

vectorial_inline simd2f simd2f_rsqrt1(simd2f v) {
    simd2f estimate = vrsqrte_f32(v);
    simd2f_rsqrt_1iteration(v, estimate);
    return estimate;
}

vectorial_inline simd2f simd2f_rsqrt2(simd2f v) {
    simd2f estimate = vrsqrte_f32(v);
    simd2f_rsqrt_1iteration(v, estimate);
    simd2f_rsqrt_1iteration(v, estimate);
    return estimate;
}

vectorial_inline simd2f simd2f_rsqrt3(simd2f v) {
    simd2f estimate = vrsqrte_f32(v);
    simd2f_rsqrt_1iteration(v, estimate);
    simd2f_rsqrt_1iteration(v, estimate);
    simd2f_rsqrt_1iteration(v, estimate);
    return estimate;
}

// http://en.wikipedia.org/wiki/Fast_inverse_square_root makes the argument for
// one iteration but two gives a signficant accuracy improvment.
vectorial_inline simd2f simd2f_rsqrt(simd2f v) {
    return simd2f_rsqrt2(v);
}

vectorial_inline simd2f simd2f_sqrt(simd2f v) {

    return vreinterpret_f32_u32(vand_u32( vtst_u32(vreinterpret_u32_f32(v),
                                                      vreinterpret_u32_f32(v)),
                                            vreinterpret_u32_f32(
                                              simd2f_reciprocal(simd2f_rsqrt(v)))
                                          )
                                );

}

// arithmetics

vectorial_inline simd2f simd2f_add(simd2f lhs, simd2f rhs) {
    simd2f ret = vadd_f32(lhs, rhs);
    return ret;
}

vectorial_inline simd2f simd2f_sub(simd2f lhs, simd2f rhs) {
    simd2f ret = vsub_f32(lhs, rhs);
    return ret;
}

vectorial_inline simd2f simd2f_mul(simd2f lhs, simd2f rhs) {
    simd2f ret = vmul_f32(lhs, rhs);
    return ret;
}

vectorial_inline simd2f simd2f_div(simd2f lhs, simd2f rhs) {
    simd2f recip = simd2f_reciprocal( rhs );
    simd2f ret = vmul_f32(lhs, recip);
    return ret;
}

vectorial_inline simd2f simd2f_madd(simd2f m1, simd2f m2, simd2f a) {
    return vmla_f32( a, m1, m2 );
}

vectorial_inline float simd2f_get_x(simd2f s) { return vget_lane_f32(s, 0); }
vectorial_inline float simd2f_get_y(simd2f s) { return vget_lane_f32(s, 1); }

vectorial_inline simd2f simd2f_dot2(simd2f lhs, simd2f rhs) {
    const simd2f m = simd2f_mul(lhs, rhs);
    return vpadd_f32(m, m);
}

vectorial_inline simd2f simd2f_min(simd2f a, simd2f b) {
    return vmin_f32( a, b );
}

vectorial_inline simd2f simd2f_max(simd2f a, simd2f b) {
    return vmax_f32( a, b );
}


#ifdef __cplusplus
}
#endif


#endif

