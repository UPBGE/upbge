/*
  Vectorial
  Copyright (c) 2010 Mikko Lehtonen
  Licensed under the terms of the two-clause BSD License (see LICENSE)
*/
#ifndef VECTORIAL_VEC4F_H
#define VECTORIAL_VEC4F_H

#ifndef VECTORIAL_SIMD4F_H
  #include "vectorial/simd4f.h"
#endif



namespace vectorial {
    
    class vec3f;
    class vec2f;

    class vec4f {
    public:

        simd4f value;
    
        inline vec4f() {}
        inline vec4f(const vec4f& v) : value(v.value) {}
        inline vec4f(const simd4f& v) : value(v) {}
        explicit inline vec4f(float xyzw) : value( simd4f_splat(xyzw) ) {}
        inline vec4f(float x, float y, float z, float w) : value( simd4f_create(x,y,z,w) ) {}
        explicit inline vec4f(const float *ary) : value( simd4f_uload4(ary) ) { }
            
        inline float x() const { return simd4f_get_x(value); }
        inline float y() const { return simd4f_get_y(value); }
        inline float z() const { return simd4f_get_z(value); }
        inline float w() const { return simd4f_get_w(value); }

        inline void load(const float *ary) { value = simd4f_uload4(ary); }
        inline void store(float *ary) const { simd4f_ustore4(value, ary); }
        
        enum { elements = 4 };


        static vec4f zero() { return vec4f(simd4f_zero()); }
        static vec4f one() { return vec4f(1.0f); }
        static vec4f xAxis() { return vec4f(1.0f, 0.0f, 0.0f, 0.0f); }
        static vec4f yAxis() { return vec4f(0.0f, 1.0f, 0.0f, 0.0f); }
        static vec4f zAxis() { return vec4f(0.0f, 0.0f, 1.0f, 0.0f); }
        static vec4f wAxis() { return vec4f(0.0f, 0.0f, 0.0f, 1.0f); }


        inline vec3f xyz() const;
        inline vec2f xy() const;

    };


    vectorial_inline vec4f operator-(const vec4f& lhs) {
        return vec4f( simd4f_sub(simd4f_zero(), lhs.value) );
    }


    vectorial_inline vec4f operator+(const vec4f& lhs, const vec4f& rhs) {
        return vec4f( simd4f_add(lhs.value, rhs.value) );
    }

    vectorial_inline vec4f operator-(const vec4f& lhs, const vec4f& rhs) {
        return vec4f( simd4f_sub(lhs.value, rhs.value) );
    }

    vectorial_inline vec4f operator*(const vec4f& lhs, const vec4f& rhs) {
        return vec4f( simd4f_mul(lhs.value, rhs.value) );
    }

    vectorial_inline vec4f operator/(const vec4f& lhs, const vec4f& rhs) {
        return vec4f( simd4f_div(lhs.value, rhs.value) );
    }


    vectorial_inline vec4f operator+=(vec4f& lhs, const vec4f& rhs) {
        return lhs = vec4f( simd4f_add(lhs.value, rhs.value) );
    }

    vectorial_inline vec4f operator-=(vec4f& lhs, const vec4f& rhs) {
        return lhs = vec4f( simd4f_sub(lhs.value, rhs.value) );
    }

    vectorial_inline vec4f operator*=(vec4f& lhs, const vec4f& rhs) {
        return lhs = vec4f( simd4f_mul(lhs.value, rhs.value) );
    }

    vectorial_inline vec4f operator/=(vec4f& lhs, const vec4f& rhs) {
        return lhs = vec4f( simd4f_div(lhs.value, rhs.value) );
    }



    vectorial_inline vec4f operator+(const vec4f& lhs, float rhs) {
        return vec4f( simd4f_add(lhs.value, simd4f_splat(rhs)) );
    }

    vectorial_inline vec4f operator-(const vec4f& lhs, float rhs) {
        return vec4f( simd4f_sub(lhs.value, simd4f_splat(rhs)) );
    }

    vectorial_inline vec4f operator*(const vec4f& lhs, float rhs) {
        return vec4f( simd4f_mul(lhs.value, simd4f_splat(rhs)) );
    }

    vectorial_inline vec4f operator/(const vec4f& lhs, float rhs) {
        return vec4f( simd4f_div(lhs.value, simd4f_splat(rhs)) );
    }

    vectorial_inline vec4f operator+(float lhs, const vec4f& rhs) {
        return vec4f( simd4f_add(simd4f_splat(lhs), rhs.value) );
    }

    vectorial_inline vec4f operator-(float lhs, const vec4f& rhs) {
        return vec4f( simd4f_sub(simd4f_splat(lhs), rhs.value) );
    }

    vectorial_inline vec4f operator*(float lhs, const vec4f& rhs) {
        return vec4f( simd4f_mul(simd4f_splat(lhs), rhs.value) );
    }

    vectorial_inline vec4f operator/(float lhs, const vec4f& rhs) {
        return vec4f( simd4f_div(simd4f_splat(lhs), rhs.value) );
    }


    vectorial_inline vec4f operator+=(vec4f& lhs, float rhs) {
        return lhs = vec4f( simd4f_add(lhs.value, simd4f_splat(rhs)) );
    }

    vectorial_inline vec4f operator-=(vec4f& lhs, float rhs) {
        return lhs = vec4f( simd4f_sub(lhs.value, simd4f_splat(rhs)) );
    }

    vectorial_inline vec4f operator*=(vec4f& lhs, float rhs) {
        return lhs = vec4f( simd4f_mul(lhs.value, simd4f_splat(rhs)) );
    }

    vectorial_inline vec4f operator/=(vec4f& lhs, float rhs) {
        return lhs = vec4f( simd4f_div(lhs.value, simd4f_splat(rhs)) );
    }


    vectorial_inline float dot(const vec4f& lhs, const vec4f& rhs) {
        return simd4f_get_x( simd4f_dot4(lhs.value, rhs.value) );
    }
    
    
    vectorial_inline float length(const vec4f& v) {
        return simd4f_get_x( simd4f_length4(v.value) );
    }

    vectorial_inline float length_squared(const vec4f& v) {
        return simd4f_get_x( simd4f_length4_squared(v.value) );
    }

    vectorial_inline vec4f normalize(const vec4f& v) {
        return vec4f( simd4f_normalize4(v.value) );
    }

    vectorial_inline vec4f min(const vec4f& a, const vec4f& b) {
        return vec4f( simd4f_min(a.value, b.value) );
    }

    vectorial_inline vec4f max(const vec4f& a, const vec4f& b) {
        return vec4f( simd4f_max(a.value, b.value) );
    }


}


namespace std {
    inline ::vectorial::vec4f min(const ::vectorial::vec4f& a, const ::vectorial::vec4f& b) { return ::vectorial::min(a,b); }
    inline ::vectorial::vec4f max(const ::vectorial::vec4f& a, const ::vectorial::vec4f& b) { return ::vectorial::max(a,b); }
}


#ifdef VECTORIAL_OSTREAM
#include <ostream>

vectorial_inline std::ostream& operator<<(std::ostream& os, const vectorial::vec4f& v) {
    os << "[ " << v.x() << ", "
               << v.y() << ", "
               << v.z() << ", "
               << v.w() << " ]";
    return os;
}
#endif


#endif
