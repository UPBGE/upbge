/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 *
 * Utility defines (avoid depending on `BLI_utildefines.h`).
 */

#pragma once

#include "GHOST_utildefines_variadic.h"

/* -------------------------------------------------------------------- */
/** \name Branch Prediction Macros
 * \{ */

/* hints for branch prediction, only use in code that runs a _lot_ where */
#ifdef __GNUC__
#  define LIKELY(x) __builtin_expect(!!(x), 1)
#  define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#  define LIKELY(x) (x)
#  define UNLIKELY(x) (x)
#endif

/** \} */

/* -------------------------------------------------------------------- */
/** \name Array Unpacking Macros
 * \{ */

/* unpack vector for args */
#define UNPACK2(a) ((a)[0]), ((a)[1])
#define UNPACK3(a) UNPACK2(a), ((a)[2])
#define UNPACK4(a) UNPACK3(a), ((a)[3])
/* pre may be '&', '*' or func, post may be '->member' */
#define UNPACK2_EX(pre, a, post) (pre((a)[0]) post), (pre((a)[1]) post)
#define UNPACK3_EX(pre, a, post) UNPACK2_EX(pre, a, post), (pre((a)[2]) post)
#define UNPACK4_EX(pre, a, post) UNPACK3_EX(pre, a, post), (pre((a)[3]) post)

/** \} */

/* -------------------------------------------------------------------- */
/** \name Array Macros
 * \{ */

/* Assuming a static array. */
#if defined(__GNUC__) && !defined(__cplusplus) && !defined(__clang__) && !defined(__INTEL_COMPILER)
#  define ARRAY_SIZE(arr) \
    ((sizeof(struct { int isnt_array : ((const void *)&(arr) == &(arr)[0]); }) * 0) + \
     (sizeof(arr) / sizeof(*(arr))))
#else
#  define ARRAY_SIZE(arr) (sizeof(arr) / sizeof(*(arr)))
#endif

/** \} */

/* -------------------------------------------------------------------- */
/** \name Equal to Any Element (ELEM) Macro
 * \{ */

/* Manual line breaks for readability. */
/* clang-format off */

/* ELEM#(v, ...): is the first arg equal any others? */
/* internal helpers. */
#define _VA_ELEM2(v, a) ((v) == (a))
#define _VA_ELEM3(v, a, b) \
  (_VA_ELEM2(v, a) || _VA_ELEM2(v, b))
#define _VA_ELEM4(v, a, b, c) \
  (_VA_ELEM3(v, a, b) || _VA_ELEM2(v, c))
#define _VA_ELEM5(v, a, b, c, d) \
  (_VA_ELEM4(v, a, b, c) || _VA_ELEM2(v, d))
#define _VA_ELEM6(v, a, b, c, d, e) \
  (_VA_ELEM5(v, a, b, c, d) || _VA_ELEM2(v, e))
#define _VA_ELEM7(v, a, b, c, d, e, f) \
  (_VA_ELEM6(v, a, b, c, d, e) || _VA_ELEM2(v, f))
#define _VA_ELEM8(v, a, b, c, d, e, f, g) \
  (_VA_ELEM7(v, a, b, c, d, e, f) || _VA_ELEM2(v, g))
#define _VA_ELEM9(v, a, b, c, d, e, f, g, h) \
  (_VA_ELEM8(v, a, b, c, d, e, f, g) || _VA_ELEM2(v, h))
#define _VA_ELEM10(v, a, b, c, d, e, f, g, h, i) \
  (_VA_ELEM9(v, a, b, c, d, e, f, g, h) || _VA_ELEM2(v, i))
#define _VA_ELEM11(v, a, b, c, d, e, f, g, h, i, j) \
  (_VA_ELEM10(v, a, b, c, d, e, f, g, h, i) || _VA_ELEM2(v, j))
#define _VA_ELEM12(v, a, b, c, d, e, f, g, h, i, j, k) \
  (_VA_ELEM11(v, a, b, c, d, e, f, g, h, i, j) || _VA_ELEM2(v, k))
#define _VA_ELEM13(v, a, b, c, d, e, f, g, h, i, j, k, l) \
  (_VA_ELEM12(v, a, b, c, d, e, f, g, h, i, j, k) || _VA_ELEM2(v, l))
#define _VA_ELEM14(v, a, b, c, d, e, f, g, h, i, j, k, l, m) \
  (_VA_ELEM13(v, a, b, c, d, e, f, g, h, i, j, k, l) || _VA_ELEM2(v, m))
#define _VA_ELEM15(v, a, b, c, d, e, f, g, h, i, j, k, l, m, n) \
  (_VA_ELEM14(v, a, b, c, d, e, f, g, h, i, j, k, l, m) || _VA_ELEM2(v, n))
#define _VA_ELEM16(v, a, b, c, d, e, f, g, h, i, j, k, l, m, n, o) \
  (_VA_ELEM15(v, a, b, c, d, e, f, g, h, i, j, k, l, m, n) || _VA_ELEM2(v, o))
#define _VA_ELEM17(v, a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p) \
  (_VA_ELEM16(v, a, b, c, d, e, f, g, h, i, j, k, l, m, n, o) || _VA_ELEM2(v, p))
/* clang-format on */

/* reusable ELEM macro */
#define ELEM(...) VA_NARGS_CALL_OVERLOAD(_VA_ELEM, __VA_ARGS__)

/** \} */

/* -------------------------------------------------------------------- */
/** \name Clamp Macros
 * \{ */

#define CLAMPIS(a, b, c) ((a) < (b) ? (b) : (a) > (c) ? (c) : (a))

#define CLAMP(a, b, c) \
  { \
    if ((a) < (b)) { \
      (a) = (b); \
    } \
    else if ((a) > (c)) { \
      (a) = (c); \
    } \
  } \
  (void)0

#define CLAMP_MAX(a, c) \
  { \
    if ((a) > (c)) { \
      (a) = (c); \
    } \
  } \
  (void)0

#define CLAMP_MIN(a, b) \
  { \
    if ((a) < (b)) { \
      (a) = (b); \
    } \
  } \
  (void)0

#define CLAMP2(vec, b, c) \
  { \
    CLAMP((vec)[0], b, c); \
    CLAMP((vec)[1], b, c); \
  } \
  (void)0

#define CLAMP2_MIN(vec, b) \
  { \
    CLAMP_MIN((vec)[0], b); \
    CLAMP_MIN((vec)[1], b); \
  } \
  (void)0

#define CLAMP2_MAX(vec, b) \
  { \
    CLAMP_MAX((vec)[0], b); \
    CLAMP_MAX((vec)[1], b); \
  } \
  (void)0

#define CLAMP3(vec, b, c) \
  { \
    CLAMP((vec)[0], b, c); \
    CLAMP((vec)[1], b, c); \
    CLAMP((vec)[2], b, c); \
  } \
  (void)0

#define CLAMP3_MIN(vec, b) \
  { \
    CLAMP_MIN((vec)[0], b); \
    CLAMP_MIN((vec)[1], b); \
    CLAMP_MIN((vec)[2], b); \
  } \
  (void)0

#define CLAMP3_MAX(vec, b) \
  { \
    CLAMP_MAX((vec)[0], b); \
    CLAMP_MAX((vec)[1], b); \
    CLAMP_MAX((vec)[2], b); \
  } \
  (void)0

#define CLAMP4(vec, b, c) \
  { \
    CLAMP((vec)[0], b, c); \
    CLAMP((vec)[1], b, c); \
    CLAMP((vec)[2], b, c); \
    CLAMP((vec)[3], b, c); \
  } \
  (void)0

#define CLAMP4_MIN(vec, b) \
  { \
    CLAMP_MIN((vec)[0], b); \
    CLAMP_MIN((vec)[1], b); \
    CLAMP_MIN((vec)[2], b); \
    CLAMP_MIN((vec)[3], b); \
  } \
  (void)0

#define CLAMP4_MAX(vec, b) \
  { \
    CLAMP_MAX((vec)[0], b); \
    CLAMP_MAX((vec)[1], b); \
    CLAMP_MAX((vec)[2], b); \
    CLAMP_MAX((vec)[3], b); \
  } \
  (void)0

/** \} */
