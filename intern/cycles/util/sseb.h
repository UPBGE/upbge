/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2011-2013 Intel Corporation
 * Modifications Copyright 2014-2022 Blender Foundation. */

#ifndef __UTIL_SSEB_H__
#define __UTIL_SSEB_H__

CCL_NAMESPACE_BEGIN

#ifdef __KERNEL_SSE2__

struct ssei;
struct ssef;

/*! 4-wide SSE bool type. */
struct sseb {
  typedef sseb Mask;   // mask type
  typedef ssei Int;    // int type
  typedef ssef Float;  // float type

  enum { size = 4 };  // number of SIMD elements
  union {
    __m128 m128;
    int32_t v[4];
  };  // data

  ////////////////////////////////////////////////////////////////////////////////
  /// Constructors, Assignment & Cast Operators
  ////////////////////////////////////////////////////////////////////////////////

  __forceinline sseb()
  {
  }
  __forceinline sseb(const sseb &other)
  {
    m128 = other.m128;
  }
  __forceinline sseb &operator=(const sseb &other)
  {
    m128 = other.m128;
    return *this;
  }

  __forceinline sseb(const __m128 input) : m128(input)
  {
  }
  __forceinline operator const __m128 &(void) const
  {
    return m128;
  }
  __forceinline operator const __m128i(void) const
  {
    return _mm_castps_si128(m128);
  }
  __forceinline operator const __m128d(void) const
  {
    return _mm_castps_pd(m128);
  }

  __forceinline sseb(bool a)
      : m128(_mm_lookupmask_ps[(size_t(a) << 3) | (size_t(a) << 2) | (size_t(a) << 1) | size_t(a)])
  {
  }
  __forceinline sseb(bool a, bool b)
      : m128(_mm_lookupmask_ps[(size_t(b) << 3) | (size_t(a) << 2) | (size_t(b) << 1) | size_t(a)])
  {
  }
  __forceinline sseb(bool a, bool b, bool c, bool d)
      : m128(_mm_lookupmask_ps[(size_t(d) << 3) | (size_t(c) << 2) | (size_t(b) << 1) | size_t(a)])
  {
  }
  __forceinline sseb(int mask)
  {
    assert(mask >= 0 && mask < 16);
    m128 = _mm_lookupmask_ps[mask];
  }

  ////////////////////////////////////////////////////////////////////////////////
  /// Constants
  ////////////////////////////////////////////////////////////////////////////////

  __forceinline sseb(FalseTy) : m128(_mm_setzero_ps())
  {
  }
  __forceinline sseb(TrueTy)
      : m128(_mm_castsi128_ps(_mm_cmpeq_epi32(_mm_setzero_si128(), _mm_setzero_si128())))
  {
  }

  ////////////////////////////////////////////////////////////////////////////////
  /// Array Access
  ////////////////////////////////////////////////////////////////////////////////

  __forceinline bool operator[](const size_t i) const
  {
    assert(i < 4);
    return (_mm_movemask_ps(m128) >> i) & 1;
  }
  __forceinline int32_t &operator[](const size_t i)
  {
    assert(i < 4);
    return v[i];
  }
};

////////////////////////////////////////////////////////////////////////////////
/// Unary Operators
////////////////////////////////////////////////////////////////////////////////

__forceinline const sseb operator!(const sseb &a)
{
  return _mm_xor_ps(a, sseb(True));
}

////////////////////////////////////////////////////////////////////////////////
/// Binary Operators
////////////////////////////////////////////////////////////////////////////////

__forceinline const sseb operator&(const sseb &a, const sseb &b)
{
  return _mm_and_ps(a, b);
}
__forceinline const sseb operator|(const sseb &a, const sseb &b)
{
  return _mm_or_ps(a, b);
}
__forceinline const sseb operator^(const sseb &a, const sseb &b)
{
  return _mm_xor_ps(a, b);
}

////////////////////////////////////////////////////////////////////////////////
/// Assignment Operators
////////////////////////////////////////////////////////////////////////////////

__forceinline const sseb operator&=(sseb &a, const sseb &b)
{
  return a = a & b;
}
__forceinline const sseb operator|=(sseb &a, const sseb &b)
{
  return a = a | b;
}
__forceinline const sseb operator^=(sseb &a, const sseb &b)
{
  return a = a ^ b;
}

////////////////////////////////////////////////////////////////////////////////
/// Comparison Operators + Select
////////////////////////////////////////////////////////////////////////////////

__forceinline const sseb operator!=(const sseb &a, const sseb &b)
{
  return _mm_xor_ps(a, b);
}
__forceinline const sseb operator==(const sseb &a, const sseb &b)
{
  return _mm_castsi128_ps(_mm_cmpeq_epi32(a, b));
}

__forceinline const sseb select(const sseb &m, const sseb &t, const sseb &f)
{
#  if defined(__KERNEL_SSE41__)
  return _mm_blendv_ps(f, t, m);
#  else
  return _mm_or_ps(_mm_and_ps(m, t), _mm_andnot_ps(m, f));
#  endif
}

////////////////////////////////////////////////////////////////////////////////
/// Movement/Shifting/Shuffling Functions
////////////////////////////////////////////////////////////////////////////////

__forceinline const sseb unpacklo(const sseb &a, const sseb &b)
{
  return _mm_unpacklo_ps(a, b);
}
__forceinline const sseb unpackhi(const sseb &a, const sseb &b)
{
  return _mm_unpackhi_ps(a, b);
}

template<size_t i0, size_t i1, size_t i2, size_t i3>
__forceinline const sseb shuffle(const sseb &a)
{
#  ifdef __KERNEL_NEON__
  return shuffle_neon<int32x4_t, i0, i1, i2, i3>(a);
#  else
  return _mm_castsi128_ps(_mm_shuffle_epi32(a, _MM_SHUFFLE(i3, i2, i1, i0)));
#  endif
}

#  ifndef __KERNEL_NEON__
template<> __forceinline const sseb shuffle<0, 1, 0, 1>(const sseb &a)
{
  return _mm_movelh_ps(a, a);
}

template<> __forceinline const sseb shuffle<2, 3, 2, 3>(const sseb &a)
{
  return _mm_movehl_ps(a, a);
}
#  endif

template<size_t i0, size_t i1, size_t i2, size_t i3>
__forceinline const sseb shuffle(const sseb &a, const sseb &b)
{
#  ifdef __KERNEL_NEON__
  return shuffle_neon<int32x4_t, i0, i1, i2, i3>(a, b);
#  else
  return _mm_shuffle_ps(a, b, _MM_SHUFFLE(i3, i2, i1, i0));
#  endif
}

#  ifndef __KERNEL_NEON__
template<> __forceinline const sseb shuffle<0, 1, 0, 1>(const sseb &a, const sseb &b)
{
  return _mm_movelh_ps(a, b);
}

template<> __forceinline const sseb shuffle<2, 3, 2, 3>(const sseb &a, const sseb &b)
{
  return _mm_movehl_ps(b, a);
}
#  endif

#  if defined(__KERNEL_SSE3__) && !defined(__KERNEL_NEON__)
template<> __forceinline const sseb shuffle<0, 0, 2, 2>(const sseb &a)
{
  return _mm_moveldup_ps(a);
}
template<> __forceinline const sseb shuffle<1, 1, 3, 3>(const sseb &a)
{
  return _mm_movehdup_ps(a);
}
#  endif

#  if defined(__KERNEL_SSE41__)
template<size_t dst, size_t src, size_t clr>
__forceinline const sseb insert(const sseb &a, const sseb &b)
{
#    ifdef __KERNEL_NEON__
  sseb res = a;
  if (clr)
    res[dst] = 0;
  else
    res[dst] = b[src];
  return res;
#    else
  return _mm_insert_ps(a, b, (dst << 4) | (src << 6) | clr);
#    endif
}
template<size_t dst, size_t src> __forceinline const sseb insert(const sseb &a, const sseb &b)
{
  return insert<dst, src, 0>(a, b);
}
template<size_t dst> __forceinline const sseb insert(const sseb &a, const bool b)
{
  return insert<dst, 0>(a, sseb(b));
}
#  endif

////////////////////////////////////////////////////////////////////////////////
/// Reduction Operations
////////////////////////////////////////////////////////////////////////////////

#  if defined(__KERNEL_SSE41__)
__forceinline uint32_t popcnt(const sseb &a)
{
#    if defined(__KERNEL_NEON__)
  const int32x4_t mask = {1, 1, 1, 1};
  int32x4_t t = vandq_s32(vreinterpretq_s32_m128(a.m128), mask);
  return vaddvq_s32(t);
#    else
  return _mm_popcnt_u32(_mm_movemask_ps(a));
#    endif
}
#  else
__forceinline uint32_t popcnt(const sseb &a)
{
  return bool(a[0]) + bool(a[1]) + bool(a[2]) + bool(a[3]);
}
#  endif

__forceinline bool reduce_and(const sseb &a)
{
#  if defined(__KERNEL_NEON__)
  return vaddvq_s32(vreinterpretq_s32_m128(a.m128)) == -4;
#  else
  return _mm_movemask_ps(a) == 0xf;
#  endif
}
__forceinline bool reduce_or(const sseb &a)
{
#  if defined(__KERNEL_NEON__)
  return vaddvq_s32(vreinterpretq_s32_m128(a.m128)) != 0x0;
#  else
  return _mm_movemask_ps(a) != 0x0;
#  endif
}
__forceinline bool all(const sseb &b)
{
#  if defined(__KERNEL_NEON__)
  return vaddvq_s32(vreinterpretq_s32_m128(b.m128)) == -4;
#  else
  return _mm_movemask_ps(b) == 0xf;
#  endif
}
__forceinline bool any(const sseb &b)
{
#  if defined(__KERNEL_NEON__)
  return vaddvq_s32(vreinterpretq_s32_m128(b.m128)) != 0x0;
#  else
  return _mm_movemask_ps(b) != 0x0;
#  endif
}
__forceinline bool none(const sseb &b)
{
#  if defined(__KERNEL_NEON__)
  return vaddvq_s32(vreinterpretq_s32_m128(b.m128)) == 0x0;
#  else
  return _mm_movemask_ps(b) == 0x0;
#  endif
}

__forceinline uint32_t movemask(const sseb &a)
{
  return _mm_movemask_ps(a);
}

////////////////////////////////////////////////////////////////////////////////
/// Debug Functions
////////////////////////////////////////////////////////////////////////////////

ccl_device_inline void print_sseb(const char *label, const sseb &a)
{
  printf("%s: %d %d %d %d\n", label, a[0], a[1], a[2], a[3]);
}

#endif

CCL_NAMESPACE_END

#endif
