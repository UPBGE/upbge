/*
  Vectorial
  Copyright (c) 2014 Google
  Licensed under the terms of the two-clause BSD License (see LICENSE)
*/
#ifndef VECTORIAL_SIMD2F_COMMON_H
#define VECTORIAL_SIMD2F_COMMON_H

vectorial_inline simd2f simd2f_length2(simd2f v) {
    return simd2f_sqrt( simd2f_dot2(v,v) );
}

vectorial_inline simd2f simd2f_length2_squared(simd2f v) {
    return simd2f_dot2(v,v);
}

vectorial_inline simd2f simd2f_normalize2(simd2f a) {
    simd2f invlen = simd2f_rsqrt( simd2f_dot2(a,a) );
    return simd2f_mul(a, invlen);
}

#endif
