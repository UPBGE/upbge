/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup freestyle
 * \brief Pseudo-random number generator
 */

#include "RandGen.h"

namespace Freestyle {

//
// Macro definitions
//
///////////////////////////////////////////////////////////////////////////////

#define N 16
#define MASK ((unsigned)(1 << (N - 1)) + (1 << (N - 1)) - 1)
#define X0 0x330E
#define X1 0xABCD
#define X2 0x1234
#define A0 0xE66D
#define A1 0xDEEC
#define A2 0x5
#define C 0xB
#if 0  // XXX Unused
#  define HI_BIT (1L << (2 * N - 1))
#endif

#define LOW(x) ((unsigned)(x)&MASK)
#define HIGH(x) LOW((x) >> N)

#define MUL(x, y, z) \
  { \
    long l = (long)(x) * (long)(y); \
    (z)[0] = LOW(l); \
    (z)[1] = HIGH(l); \
  } \
  ((void)0)

#define CARRY(x, y) ((unsigned long)((long)(x) + (long)(y)) > MASK)
#define ADDEQU(x, y, z) (z = CARRY(x, (y)), x = LOW(x + (y)))
#define SET3(x, x0, x1, x2) ((x)[0] = (x0), (x)[1] = (x1), (x)[2] = (x2))
#if 0  // XXX, unused
#  define SETLOW(x, y, n) SET3(x, LOW((y)[n]), LOW((y)[(n) + 1]), LOW((y)[(n) + 2]))
#endif
#define SEED(x0, x1, x2) (SET3(x, x0, x1, x2), SET3(a, A0, A1, A2), c = C)

#if 0  // XXX, unused
#  define REST(v) \
    for (i = 0; i < 3; i++) { \
      xsubi[i] = x[i]; \
      x[i] = temp[i]; \
    } \
    return (v); \
    (void)0

#  define NEST(TYPE, f, F) \
    TYPE f(unsigned short *xsubi) \
    { \
      int i; \
      TYPE v; \
      unsigned temp[3]; \
      for (i = 0; i < 3; i++) { \
        temp[i] = x[i]; \
        x[i] = LOW(xsubi[i]); \
      } \
      v = F(); \
      REST(v); \
    }
#endif

static unsigned x[3] = {
    X0,
    X1,
    X2,
};
static unsigned a[3] = {
    A0,
    A1,
    A2,
};
static unsigned c = C;

//
// Methods implementation
//
///////////////////////////////////////////////////////////////////////////////

real RandGen::drand48()
{
  static real two16m = 1.0 / (1L << N);
  next();
  return (two16m * (two16m * (two16m * x[0] + x[1]) + x[2]));
}

void RandGen::srand48(long seedval)
{
  SEED(X0, LOW(seedval), HIGH(seedval));
}

void RandGen::next()
{
  unsigned p[2], q[2], r[2], carry0, carry1;

  MUL(a[0], x[0], p);
  ADDEQU(p[0], c, carry0);
  ADDEQU(p[1], carry0, carry1);
  MUL(a[0], x[1], q);
  ADDEQU(p[1], q[0], carry0);
  MUL(a[1], x[0], r);
  x[2] = LOW(carry0 + carry1 + CARRY(p[1], r[0]) + q[1] + r[1] + a[0] * x[2] + a[1] * x[1] +
             a[2] * x[0]);
  x[1] = LOW(p[1] + r[0]);
  x[0] = LOW(p[0]);
}

} /* namespace Freestyle */
