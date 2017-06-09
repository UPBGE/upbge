/*
  Vectorial
  Copyright (c) 2010 Mikko Lehtonen
  Copyright (c) 2014 Google, Inc.
  Licensed under the terms of the two-clause BSD License (see LICENSE)
*/
#ifndef VECTORIAL_SIMD4X4F_H
#define VECTORIAL_SIMD4X4F_H


#include "simd4f.h"

#include <math.h>

/*
  Note, x,y,z,w are conceptually columns with matrix math.
*/

typedef struct {
    simd4f x,y,z,w;
} simd4x4f;



vectorial_inline simd4x4f simd4x4f_create(simd4f x, simd4f y, simd4f z, SIMD_PARAM(simd4f, w)) {
    simd4x4f s = { x, y, z, w };
    return s;
}


vectorial_inline void simd4x4f_identity(simd4x4f* m) {
    *m = simd4x4f_create( simd4f_create(1.0f, 0.0f, 0.0f, 0.0f),
                          simd4f_create(0.0f, 1.0f, 0.0f, 0.0f),
                          simd4f_create(0.0f, 0.0f, 1.0f, 0.0f),
                          simd4f_create(0.0f, 0.0f, 0.0f, 1.0f));
}



vectorial_inline void simd4x4f_uload(simd4x4f* m, const float *f) {

    m->x = simd4f_uload4(f + 0);
    m->y = simd4f_uload4(f + 4);
    m->z = simd4f_uload4(f + 8);
    m->w = simd4f_uload4(f + 12);

}





#ifdef VECTORIAL_SCALAR
    #include "simd4x4f_scalar.h"
#elif defined(VECTORIAL_SSE)
    #include "simd4x4f_sse.h"
#elif defined(VECTORIAL_GNU)
    #include "simd4x4f_gnu.h"
#elif defined(VECTORIAL_NEON)
    #include "simd4x4f_neon.h"
#else
    #error No implementation defined
#endif

vectorial_inline void simd4x4f_sum(const simd4x4f* a, simd4f* out) {
    simd4f t;
    t = simd4f_add(a->x, a->y);
    t = simd4f_add(t, a->z);
    t = simd4f_add(t, a->w);
    *out = t;
}

vectorial_inline void simd4x4f_matrix_vector_mul(const simd4x4f* a, const simd4f * b, simd4f* out) {

    const simd4f x = a->x;
    const simd4f y = a->y;
    const simd4f z = a->z;
    const simd4f w = a->w;
    const simd4f v = *b;
    const simd4f vx = simd4f_splat_x(v);
    const simd4f vy = simd4f_splat_y(v);
    const simd4f vz = simd4f_splat_z(v);
    const simd4f vw = simd4f_splat_w(v);

    #if 0
    // In a hasty benchmark, this actually performed worse on neon
    // TODO: revisit and conditionalize accordingly

    *out = simd4f_madd(x, vx, 
             simd4f_madd(y, vy, 
               simd4f_madd(z, vz, 
                 simd4f_mul(w, vw) ) ) );

    #else    

     *out = simd4f_add(simd4f_mul(x, vx), 
              simd4f_add(simd4f_mul(y, vy), 
                simd4f_add(simd4f_mul(z, vz), 
                  simd4f_mul(w, vw) ) ) );

    #endif
}

vectorial_inline void simd4x4f_matrix_vector3_mul(const simd4x4f* a, const simd4f * b, simd4f* out) {

    #if 0
    *out = simd4f_madd( a->x, simd4f_splat_x(*b), 
             simd4f_madd( a->y, simd4f_splat_y(*b), 
               simd4f_mul(a->z, simd4f_splat_z(*b)) ) );
    #else
    *out = simd4f_add( simd4f_mul(a->x, simd4f_splat_x(*b)), 
             simd4f_add( simd4f_mul(a->y, simd4f_splat_y(*b)), 
               simd4f_mul(a->z, simd4f_splat_z(*b)) ) );
    #endif

}

vectorial_inline void simd4x4f_matrix_point3_mul(const simd4x4f* a, const simd4f * b, simd4f* out) {

    #if 0
    *out = simd4f_madd( a->x, simd4f_splat_x(*b),
             simd4f_madd( a->y, simd4f_splat_y(*b),
               simd4f_madd( a->z, simd4f_splat_z(*b),
                 a->w ) ) );
    #else
    *out = simd4f_add( simd4f_mul(a->x, simd4f_splat_x(*b)),
             simd4f_add( simd4f_mul(a->y, simd4f_splat_y(*b)),
               simd4f_add( simd4f_mul(a->z, simd4f_splat_z(*b)),
                 a->w ) ) );
    #endif

}

vectorial_inline void simd4x4f_inv_ortho_matrix_point3_mul(const simd4x4f* a, const simd4f * b, simd4f* out) {
    simd4f translation = simd4f_sub(*b, a->w);

    simd4x4f transpose = *a;

    transpose.w = simd4f_create(0,0,0,0);
    simd4x4f_transpose_inplace(&transpose);

    simd4x4f_matrix_point3_mul(&transpose, &translation, out);
}

vectorial_inline void simd4x4f_inv_ortho_matrix_vector3_mul(const simd4x4f* a, const simd4f * b, simd4f* out) {
    simd4f translation = *b;

    simd4x4f transpose = *a;

    transpose.w = simd4f_create(0,0,0,0);
    simd4x4f_transpose_inplace(&transpose);

    simd4x4f_matrix_vector3_mul(&transpose, &translation, out);
}


vectorial_inline void simd4x4f_matrix_mul(const simd4x4f* a, const simd4x4f* b, simd4x4f* out) {

    simd4x4f_matrix_vector_mul(a, &b->x, &out->x);
    simd4x4f_matrix_vector_mul(a, &b->y, &out->y);
    simd4x4f_matrix_vector_mul(a, &b->z, &out->z);
    simd4x4f_matrix_vector_mul(a, &b->w, &out->w);

}




vectorial_inline void simd4x4f_perspective(simd4x4f *m, float fovy_radians, float aspect, float znear, float zfar) {
    
    float deltaz = zfar - znear;
    float cotangent = tanf( VECTORIAL_HALFPI - fovy_radians * 0.5f );
    
    float a = cotangent / aspect;
    float b = cotangent;
    float c = -(zfar + znear) / deltaz;
    float d = -2 * znear * zfar / deltaz;
    
    m->x = simd4f_create( a, 0, 0,  0);
    m->y = simd4f_create( 0, b, 0,  0);
    m->z = simd4f_create( 0, 0, c, -1);
    m->w = simd4f_create( 0, 0, d,  0);

}

vectorial_inline void simd4x4f_ortho(simd4x4f *m, float left, float right, float bottom, float top, float znear, float zfar) {
    
    float deltax = right - left;
    float deltay = top - bottom;
    float deltaz = zfar - znear;

    float a = 2.0f / deltax;
    float b = -(right + left) / deltax;
    float c = 2.0f / deltay;
    float d = -(top + bottom) / deltay;
    float e =  -2.0f / deltaz;
    float f = -(zfar + znear) / deltaz;
    
    m->x = simd4f_create( a, 0, 0, 0);
    m->y = simd4f_create( 0, c, 0, 0);
    m->z = simd4f_create( 0, 0, e, 0);
    m->w = simd4f_create( b, d, f, 1);
    
}


vectorial_inline void simd4x4f_lookat(simd4x4f *m, simd4f eye, simd4f center, simd4f up) {
    
    simd4f zaxis = simd4f_normalize3( simd4f_sub(center, eye) );
    simd4f xaxis = simd4f_normalize3( simd4f_cross3( zaxis, up ) );
    simd4f yaxis = simd4f_cross3(xaxis, zaxis);

    zaxis = simd4f_sub( simd4f_zero(), zaxis);

    float x = -simd4f_dot3_scalar(xaxis, eye);
    float y = -simd4f_dot3_scalar(yaxis, eye);
    float z = -simd4f_dot3_scalar(zaxis, eye);

    m->x = xaxis;
    m->y = yaxis;
    m->z = zaxis;

    m->w = simd4f_create( 0,0,0, 1);
    simd4x4f_transpose_inplace(m);
    m->w = simd4f_create( x,y,z,1);

}


vectorial_inline void simd4x4f_translation(simd4x4f* m, float x, float y, float z) {
    *m = simd4x4f_create( simd4f_create(1.0f, 0.0f, 0.0f, 0.0f),
                          simd4f_create(0.0f, 1.0f, 0.0f, 0.0f),
                          simd4f_create(0.0f, 0.0f, 1.0f, 0.0f),
                          simd4f_create(   x,    y,    z, 1.0f));
}


vectorial_inline void simd4x4f_axis_rotation(simd4x4f* m, float radians, simd4f axis) {

    radians = -radians;

    axis = simd4f_normalize3(axis);

    const float sine = sinf(radians);
    const float cosine = cosf(radians);

    const float x = simd4f_get_x(axis);
    const float y = simd4f_get_y(axis);
    const float z = simd4f_get_z(axis);

    const float ab = x * y * (1 - cosine);
    const float bc = y * z * (1 - cosine);
    const float ca = z * x * (1 - cosine);

    const float tx = x * x;
    const float ty = y * y;
    const float tz = z * z;

    const simd4f i = simd4f_create( tx + cosine * (1 - tx), ab - z * sine,          ca + y * sine,          0);
    const simd4f j = simd4f_create( ab + z * sine,          ty + cosine * (1 - ty), bc - x * sine,          0);
    const simd4f k = simd4f_create( ca - y * sine,          bc + x * sine,          tz + cosine * (1 - tz), 0);
    
    *m = simd4x4f_create( i,j,k, simd4f_create(0, 0, 0, 1) );
        
}



vectorial_inline void simd4x4f_add(const simd4x4f* a, const simd4x4f* b, simd4x4f* out) {
    
    out->x = simd4f_add(a->x, b->x);
    out->y = simd4f_add(a->y, b->y);
    out->z = simd4f_add(a->z, b->z);
    out->w = simd4f_add(a->w, b->w);
    
}

vectorial_inline void simd4x4f_sub(const simd4x4f* a, const simd4x4f* b, simd4x4f* out) {
    
    out->x = simd4f_sub(a->x, b->x);
    out->y = simd4f_sub(a->y, b->y);
    out->z = simd4f_sub(a->z, b->z);
    out->w = simd4f_sub(a->w, b->w);
    
}

vectorial_inline void simd4x4f_mul(const simd4x4f* a, const simd4x4f* b, simd4x4f* out) {
    
    out->x = simd4f_mul(a->x, b->x);
    out->y = simd4f_mul(a->y, b->y);
    out->z = simd4f_mul(a->z, b->z);
    out->w = simd4f_mul(a->w, b->w);
    
}

vectorial_inline void simd4x4f_div(simd4x4f* a, simd4x4f* b, simd4x4f* out) {
    
    out->x = simd4f_div(a->x, b->x);
    out->y = simd4f_div(a->y, b->y);
    out->z = simd4f_div(a->z, b->z);
    out->w = simd4f_div(a->w, b->w);
    
}

vectorial_inline simd4f simd4x4f_inverse(const simd4x4f* a, simd4x4f* out) {

    const simd4f c0 = a->x;
    const simd4f c1 = a->y;
    const simd4f c2 = a->z;
    const simd4f c3 = a->w;

    const simd4f c0_wxyz = simd4f_shuffle_wxyz(c0);
    const simd4f c0_zwxy = simd4f_shuffle_zwxy(c0);
    const simd4f c0_yzwx = simd4f_shuffle_yzwx(c0);

    const simd4f c1_wxyz = simd4f_shuffle_wxyz(c1);
    const simd4f c1_zwxy = simd4f_shuffle_zwxy(c1);
    const simd4f c1_yzwx = simd4f_shuffle_yzwx(c1);

    const simd4f c2_wxyz = simd4f_shuffle_wxyz(c2);
    const simd4f c2_zwxy = simd4f_shuffle_zwxy(c2);
    const simd4f c2_yzwx = simd4f_shuffle_yzwx(c2);

    const simd4f c3_wxyz = simd4f_shuffle_wxyz(c3);
    const simd4f c3_zwxy = simd4f_shuffle_zwxy(c3);
    const simd4f c3_yzwx = simd4f_shuffle_yzwx(c3);

    const simd4f c0_wxyz_x_c1 = simd4f_mul(c0_wxyz, c1);
    const simd4f c0_wxyz_x_c1_yzwx = simd4f_mul(c0_wxyz, c1_yzwx);
    const simd4f c0_wxyz_x_c1_zwxy = simd4f_mul(c0_wxyz, c1_zwxy);

    const simd4f c2_wxyz_x_c3 = simd4f_mul(c2_wxyz, c3);
    const simd4f c2_wxyz_x_c3_yzwx = simd4f_mul(c2_wxyz, c3_yzwx);
    const simd4f c2_wxyz_x_c3_zwxy = simd4f_mul(c2_wxyz, c3_zwxy);

    const simd4f ar1 = simd4f_sub( simd4f_shuffle_wxyz(c2_wxyz_x_c3_zwxy), simd4f_shuffle_zwxy(c2_wxyz_x_c3) );
    const simd4f ar2 = simd4f_sub( simd4f_shuffle_zwxy(c2_wxyz_x_c3_yzwx), c2_wxyz_x_c3_yzwx );
    const simd4f ar3 = simd4f_sub( c2_wxyz_x_c3_zwxy, simd4f_shuffle_wxyz(c2_wxyz_x_c3) );

    const simd4f br1 = simd4f_sub( simd4f_shuffle_wxyz(c0_wxyz_x_c1_zwxy), simd4f_shuffle_zwxy(c0_wxyz_x_c1) );
    const simd4f br2 = simd4f_sub( simd4f_shuffle_zwxy(c0_wxyz_x_c1_yzwx), c0_wxyz_x_c1_yzwx );
    const simd4f br3 = simd4f_sub( c0_wxyz_x_c1_zwxy, simd4f_shuffle_wxyz(c0_wxyz_x_c1) );


    const simd4f c0_sum = simd4f_madd(c0_yzwx, ar3,
                            simd4f_madd(c0_zwxy, ar2,
                              simd4f_mul(c0_wxyz, ar1)));

    const simd4f c1_sum = simd4f_madd(c1_wxyz,  ar1, 
                            simd4f_madd(c1_zwxy,  ar2, 
                              simd4f_mul(c1_yzwx, ar3)));

    const simd4f c2_sum = simd4f_madd(c2_yzwx, br3,
                            simd4f_madd(c2_zwxy, br2,
                              simd4f_mul(c2_wxyz, br1)));

    const simd4f c3_sum = simd4f_madd(c3_yzwx, br3,
                            simd4f_madd(c3_zwxy, br2,
                              simd4f_mul(c3_wxyz, br1)));


    const simd4f d0 = simd4f_mul(c1_sum, c0);
    const simd4f d1 = simd4f_add(d0, simd4f_merge_high(d0, d0));
    const simd4f det = simd4f_sub(d1, simd4f_splat_y(d1));

    const simd4f invdet = simd4f_splat_x( simd4f_div(simd4f_splat(1.0f), det) );

    const simd4f o0 = simd4f_mul( simd4f_flip_sign_0101(c1_sum), invdet );
    const simd4f o1 = simd4f_mul( simd4f_flip_sign_1010(c0_sum), invdet );
    const simd4f o2 = simd4f_mul( simd4f_flip_sign_0101(c3_sum), invdet );
    const simd4f o3 = simd4f_mul( simd4f_flip_sign_1010(c2_sum), invdet );

    const simd4x4f mt = simd4x4f_create(o0, o1, o2, o3);
    
    simd4x4f_transpose( &mt, out);

    return det;
}

#ifdef __cplusplus

    #ifdef VECTORIAL_OSTREAM
        #include <ostream>

        vectorial_inline std::ostream& operator<<(std::ostream& os, const simd4x4f& v) {
            os << "simd4x4f(simd4f(" << simd4f_get_x(v.x) << ", "
                       << simd4f_get_y(v.x) << ", "
                       << simd4f_get_z(v.x) << ", "
                       << simd4f_get_w(v.x) << "),\n"
                       << "         simd4f(" << simd4f_get_x(v.y) << ", "
                       << simd4f_get_y(v.y) << ", "
                       << simd4f_get_z(v.y) << ", "
                       << simd4f_get_w(v.y) << "),\n"
                       << "         simd4f(" << simd4f_get_x(v.z) << ", "
                       << simd4f_get_y(v.z) << ", "
                       << simd4f_get_z(v.z) << ", "
                       << simd4f_get_w(v.z) << "),\n"
                       << "         simd4f(" << simd4f_get_x(v.w) << ", "
                       << simd4f_get_y(v.w) << ", "
                       << simd4f_get_z(v.w) << ", "
                       << simd4f_get_w(v.w) << "))";
            return os;
        }
    #endif

#endif





#endif 
