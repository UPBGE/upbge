/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2020 Blender Foundation. All rights reserved. */

/* On Linux, precompiled libraries may be made with an glibc version that is
 * incompatible with the system libraries that Blender is built on. To solve
 * this we add a few -ffast-math symbols that can be missing. */

/** \file
 * \ingroup intern_libc_compat
 */

#ifdef __linux__
#  include <features.h>
#  include <math.h>

#  if defined(__GLIBC_PREREQ)
#    if __GLIBC_PREREQ(2, 31)

double __exp_finite(double x);
double __exp2_finite(double x);
double __acos_finite(double x);
double __asin_finite(double x);
double __log2_finite(double x);
double __log10_finite(double x);
double __log_finite(double x);
double __pow_finite(double x, double y);
float __expf_finite(float x);
float __exp2f_finite(float x);
float __acosf_finite(float x);
float __asinf_finite(float x);
float __log2f_finite(float x);
float __log10f_finite(float x);
float __logf_finite(float x);
float __powf_finite(float x, float y);

double __exp_finite(double x)
{
  return exp(x);
}

double __exp2_finite(double x)
{
  return exp2(x);
}

double __acos_finite(double x)
{
  return acos(x);
}

double __asin_finite(double x)
{
  return asin(x);
}

double __log2_finite(double x)
{
  return log2(x);
}

double __log10_finite(double x)
{
  return log10(x);
}

double __log_finite(double x)
{
  return log(x);
}

double __pow_finite(double x, double y)
{
  return pow(x, y);
}

float __expf_finite(float x)
{
  return expf(x);
}

float __exp2f_finite(float x)
{
  return exp2f(x);
}

float __acosf_finite(float x)
{
  return acosf(x);
}

float __asinf_finite(float x)
{
  return asinf(x);
}

float __log2f_finite(float x)
{
  return log2f(x);
}

float __log10f_finite(float x)
{
  return log10f(x);
}

float __logf_finite(float x)
{
  return logf(x);
}

float __powf_finite(float x, float y)
{
  return powf(x, y);
}

#    endif /* __GLIBC_PREREQ(2, 31) */
#  endif   /* __GLIBC_PREREQ */
#endif     /* __linux__ */
