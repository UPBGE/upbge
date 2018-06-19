/*
* Copyright 2016 Google Inc. All rights reserved.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/
#ifndef MATHFU_INTERNAL_VECTOR_2_H_
#define MATHFU_INTERNAL_VECTOR_2_H_

#include "mathfu/vector.h"

namespace mathfu {

template <class T>
class Vector<T, 2> {
 public:
  typedef T Scalar;
  static const int d = 2;

  inline Vector() {}

  inline Vector(const Vector<T, 2>& v) {
    MATHFU_VECTOR_OPERATION(data_[i] = v.data_[i]);
  }

  template <typename U>
  explicit inline Vector(const Vector<U, 2>& v) {
    MATHFU_VECTOR_OPERATION(data_[i] = static_cast<T>(v[i]));
  }

  explicit inline Vector(const T& s) { MATHFU_VECTOR_OPERATION(data_[i] = s); }

  explicit inline Vector(const T* a) {
    MATHFU_VECTOR_OPERATION(data_[i] = a[i]);
  }

  inline Vector(const T& s1, const T& s2) {
    x = s1;
    y = s2;
  }

  explicit inline Vector(const VectorPacked<T, 2>& vector) {
    MATHFU_VECTOR_OPERATION(data_[i] = vector.data[i]);
  }

  inline T& operator()(const int i) WARN_UNUSED_RESULT { return data_[i]; }

  inline const T& operator()(const int i) const WARN_UNUSED_RESULT { return data_[i]; }

  inline T& operator[](const int i) WARN_UNUSED_RESULT { return data_[i]; }

  inline const T& operator[](const int i) const WARN_UNUSED_RESULT { return data_[i]; }

  inline Vector<T, 2> xy() WARN_UNUSED_RESULT { return Vector<T, 2>(x, y); }

  inline const Vector<T, 2> xy() const WARN_UNUSED_RESULT { return Vector<T, 2>(x, y); }

  inline void Pack(VectorPacked<T, 2>* const vector) const {
    MATHFU_VECTOR_OPERATION(vector->data[i] = data_[i]);
  }

  inline void Pack(T a[2]) const {
    MATHFU_VECTOR_OPERATION(a[i] = data_[i]);
  }

  inline const T (&Data() const)[2] WARN_UNUSED_RESULT {
    return data_;
  }

  inline T LengthSquared() const WARN_UNUSED_RESULT { return LengthSquaredHelper(*this); }

  inline T Length() const WARN_UNUSED_RESULT { return LengthHelper(*this); }

  inline T Normalize() { return NormalizeHelper(*this); }

  inline T SafeNormalize() { return SafeNormalizeHelper(*this); }

  inline Vector<T, 2> Normalized() const WARN_UNUSED_RESULT { return NormalizedHelper(*this); }

  inline Vector<T, 2> SafeNormalized(const Vector<T, 2>& v) const WARN_UNUSED_RESULT {
    return SafeNormalizedHelper(*this, v);
  }

  static inline WARN_UNUSED_RESULT bool FuzzyZero(const Vector<T, 2>& v) {
    return FuzzyZeroHelper(v);
  }

  template <typename CompatibleT>
  static inline WARN_UNUSED_RESULT Vector<T, 2> FromType(const CompatibleT& compatible) {
    return FromTypeHelper<T, d, CompatibleT>(compatible);
  }

  template <typename CompatibleT>
  static inline WARN_UNUSED_RESULT CompatibleT ToType(const Vector<T, 2>& v) {
    return ToTypeHelper<T, d, CompatibleT>(v);
  }

  static inline WARN_UNUSED_RESULT T DotProduct(const Vector<T, 2>& v1, const Vector<T, 2>& v2) {
    return DotProductHelper(v1, v2);
  }

  static inline WARN_UNUSED_RESULT Vector<T, 2> HadamardProduct(const Vector<T, 2>& v1,
                                             const Vector<T, 2>& v2) {
    return HadamardProductHelper(v1, v2);
  }

  static inline WARN_UNUSED_RESULT Vector<T, 2> Lerp(const Vector<T, 2>& v1,
                                  const Vector<T, 2>& v2, const T percent) {
    return LerpHelper(v1, v2, percent);
  }

  static inline WARN_UNUSED_RESULT Vector<T, 2> RandomInRange(const Vector<T, 2>& min,
                                           const Vector<T, 2>& max) {
    return RandomInRangeHelper(min, max);
  }

  static inline WARN_UNUSED_RESULT Vector<T, 2> Max(const Vector<T, 2>& v1,
                                 const Vector<T, 2>& v2) {
    return MaxHelper(v1, v2);
  }

  static inline WARN_UNUSED_RESULT Vector<T, 2> Min(const Vector<T, 2>& v1,
                                 const Vector<T, 2>& v2) {
    return MinHelper(v1, v2);
  }

  MATHFU_DEFINE_CLASS_SIMD_AWARE_NEW_DELETE

#include "mathfu/internal/disable_warnings_begin.h"
  union {
    T data_[2];
    struct {
      T x;
      T y;
    };
  };
#include "mathfu/internal/disable_warnings_end.h"
};

template <class T>
struct VectorPacked<T, 2> {
  /// Create an uninitialized VectorPacked.
  VectorPacked() {}

  /// Create a VectorPacked from a Vector.
  ///
  /// Both VectorPacked and Vector must have the same number of dimensions.
  /// @param vector Vector to create the VectorPacked from.
  explicit VectorPacked(const Vector<T, 2>& vector) { vector.Pack(this); }

  explicit VectorPacked(const T * const s) :x(s[0]), y(s[1]) {}

  /// Copy a Vector to a VectorPacked.
  ///
  /// Both VectorPacked and Vector must have the same number of dimensions.
  /// @param vector Vector to copy to the VectorPacked.
  /// @returns A reference to this VectorPacked.
  VectorPacked& operator=(const Vector<T, 2>& vector) {
    vector.Pack(this);
    return *this;
  }

  inline const T& operator[](int i) const {
    return data[i];
  }

  inline T& operator[](int i) {
    return data[i];
  }

#include "mathfu/internal/disable_warnings_begin.h"
  /// Elements of the packed vector one per dimension.
  union {
    T data[2];
    struct {
      T x;
      T y;
    };
  };
#include "mathfu/internal/disable_warnings_end.h"
};

}  //  namespace mathfu

#endif  // MATHFU_INTERNAL_VECTOR_2_H_
