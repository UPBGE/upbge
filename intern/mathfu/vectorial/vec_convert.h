/*
  Vectorial
  Copyright (c) 2010 Mikko Lehtonen
  Licensed under the terms of the two-clause BSD License (see LICENSE)
*/
#ifndef VECTORIAL_VEC_CONVERT_H
#define VECTORIAL_VEC_CONVERT_H


namespace vectorial {
    
    inline vec3f vec4f::xyz() const { return vec3f(value); }
    inline vec2f vec4f::xy() const { return vec2f(value); }

    inline vec4f vec3f::xyz0() const { return vec4f(simd4f_zero_w(value)); }
    inline vec4f vec3f::xyz1() const { return xyz0() + vec4f(0.0f, 0.0f, 0.0f, 1.0f); }
    inline vec4f vec3f::xyzw(float w) const { return xyz0() + vec4f(0.0f, 0.0f, 0.0f, w); }
    inline vec3f vec3f::xyz() const { return vec3f(value); }
    inline vec3f vec3f::xy0() const { return vec3f(value) * vec3f(1.0f, 1.0f, 0.0f); }
    inline vec2f vec3f::xy() const { return vec2f(value); }

    inline vec4f vec2f::xy00() const { return vec4f(simd4f_zero_zw(value)); }
    inline vec4f vec2f::xy01() const { return xy00() + vec4f(0.0f, 0.0f, 0.0f, 1.0f); }
    inline vec4f vec2f::xyzw(float z, float w) const { return xy00() + vec4f(0.0f, 0.0f, z, w); }
    inline vec3f vec2f::xy0() const { return vec3f(simd4f_zero_zw(value)); }
    inline vec2f vec2f::xy() const { return vec2f(value); }

}


#endif
