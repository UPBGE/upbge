/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved. */

/** \file
 * \ingroup bke
 *
 * Based on original code by Drew Whitehouse / Houdini Ocean Toolkit
 * OpenMP hints by Christian Schnellhammer
 */

#include <math.h>
#include <stdlib.h>

#include <string.h>

#include "MEM_guardedalloc.h"

#include "DNA_modifier_types.h"
#include "DNA_scene_types.h"

#include "BLI_math.h"
#include "BLI_path_util.h"
#include "BLI_rand.h"
#include "BLI_task.h"
#include "BLI_utildefines.h"

#include "BKE_image.h"
#include "BKE_image_format.h"
#include "BKE_ocean.h"
#include "ocean_intern.h"

#include "IMB_imbuf.h"
#include "IMB_imbuf_types.h"

#include "RE_texture.h"

#include "BLI_hash.h"

#ifdef WITH_OCEANSIM

/* Ocean code */

static float nextfr(RNG *rng, float min, float max)
{
  return BLI_rng_get_float(rng) * (min - max) + max;
}

static float gaussRand(RNG *rng)
{
  /* NOTE: to avoid numerical problems with very small numbers, we make these variables
   * single-precision floats, but later we call the double-precision log() and sqrt() functions
   * instead of logf() and sqrtf(). */
  float x;
  float y;
  float length2;

  do {
    x = (float)(nextfr(rng, -1, 1));
    y = (float)(nextfr(rng, -1, 1));
    length2 = x * x + y * y;
  } while (length2 >= 1 || length2 == 0);

  return x * sqrtf(-2.0f * logf(length2) / length2);
}

/**
 * Some useful functions
 */
MINLINE float catrom(float p0, float p1, float p2, float p3, float f)
{
  return 0.5f * ((2.0f * p1) + (-p0 + p2) * f + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * f * f +
                 (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * f * f * f);
}

MINLINE float omega(float k, float depth)
{
  return sqrtf(GRAVITY * k * tanhf(k * depth));
}

/* modified Phillips spectrum */
static float Ph(struct Ocean *o, float kx, float kz)
{
  float tmp;
  float k2 = kx * kx + kz * kz;

  if (k2 == 0.0f) {
    return 0.0f; /* no DC component */
  }

  /* damp out the waves going in the direction opposite the wind */
  tmp = (o->_wx * kx + o->_wz * kz) / sqrtf(k2);
  if (tmp < 0) {
    tmp *= o->_damp_reflections;
  }

  return o->_A * expf(-1.0f / (k2 * (o->_L * o->_L))) * expf(-k2 * (o->_l * o->_l)) *
         powf(fabsf(tmp), o->_wind_alignment) / (k2 * k2);
}

static void compute_eigenstuff(struct OceanResult *ocr, float jxx, float jzz, float jxz)
{
  float a, b, qplus, qminus;
  a = jxx + jzz;
  b = sqrt((jxx - jzz) * (jxx - jzz) + 4 * jxz * jxz);

  ocr->Jminus = 0.5f * (a - b);
  ocr->Jplus = 0.5f * (a + b);

  qplus = (ocr->Jplus - jxx) / jxz;
  qminus = (ocr->Jminus - jxx) / jxz;

  a = sqrt(1 + qplus * qplus);
  b = sqrt(1 + qminus * qminus);

  ocr->Eplus[0] = 1.0f / a;
  ocr->Eplus[1] = 0.0f;
  ocr->Eplus[2] = qplus / a;

  ocr->Eminus[0] = 1.0f / b;
  ocr->Eminus[1] = 0.0f;
  ocr->Eminus[2] = qminus / b;
}

/*
 * instead of Complex.h
 * in fftw.h "fftw_complex" typedefed as double[2]
 * below you can see functions are needed to work with such complex numbers.
 */
static void init_complex(fftw_complex cmpl, float real, float image)
{
  cmpl[0] = real;
  cmpl[1] = image;
}

static void add_comlex_c(fftw_complex res, const fftw_complex cmpl1, const fftw_complex cmpl2)
{
  res[0] = cmpl1[0] + cmpl2[0];
  res[1] = cmpl1[1] + cmpl2[1];
}

static void mul_complex_f(fftw_complex res, const fftw_complex cmpl, float f)
{
  res[0] = cmpl[0] * (double)f;
  res[1] = cmpl[1] * (double)f;
}

static void mul_complex_c(fftw_complex res, const fftw_complex cmpl1, const fftw_complex cmpl2)
{
  fftwf_complex temp;
  temp[0] = cmpl1[0] * cmpl2[0] - cmpl1[1] * cmpl2[1];
  temp[1] = cmpl1[0] * cmpl2[1] + cmpl1[1] * cmpl2[0];
  res[0] = temp[0];
  res[1] = temp[1];
}

static float real_c(fftw_complex cmpl)
{
  return cmpl[0];
}

static float image_c(fftw_complex cmpl)
{
  return cmpl[1];
}

static void conj_complex(fftw_complex res, const fftw_complex cmpl1)
{
  res[0] = cmpl1[0];
  res[1] = -cmpl1[1];
}

static void exp_complex(fftw_complex res, fftw_complex cmpl)
{
  float r = expf(cmpl[0]);

  res[0] = cosf(cmpl[1]) * r;
  res[1] = sinf(cmpl[1]) * r;
}

float BKE_ocean_jminus_to_foam(float jminus, float coverage)
{
  float foam = jminus * -0.005f + coverage;
  CLAMP(foam, 0.0f, 1.0f);
  return foam;
}

void BKE_ocean_eval_uv(struct Ocean *oc, struct OceanResult *ocr, float u, float v)
{
  int i0, i1, j0, j1;
  float frac_x, frac_z;
  float uu, vv;

  /* first wrap the texture so 0 <= (u, v) < 1 */
  u = fmodf(u, 1.0f);
  v = fmodf(v, 1.0f);

  if (u < 0) {
    u += 1.0f;
  }
  if (v < 0) {
    v += 1.0f;
  }

  BLI_rw_mutex_lock(&oc->oceanmutex, THREAD_LOCK_READ);

  uu = u * oc->_M;
  vv = v * oc->_N;

  i0 = (int)floor(uu);
  j0 = (int)floor(vv);

  i1 = (i0 + 1);
  j1 = (j0 + 1);

  frac_x = uu - i0;
  frac_z = vv - j0;

  i0 = i0 % oc->_M;
  j0 = j0 % oc->_N;

  i1 = i1 % oc->_M;
  j1 = j1 % oc->_N;

#  define BILERP(m) \
    (interpf(interpf(m[i1 * oc->_N + j1], m[i0 * oc->_N + j1], frac_x), \
             interpf(m[i1 * oc->_N + j0], m[i0 * oc->_N + j0], frac_x), \
             frac_z))

  {
    if (oc->_do_disp_y) {
      ocr->disp[1] = BILERP(oc->_disp_y);
    }

    if (oc->_do_normals) {
      ocr->normal[0] = BILERP(oc->_N_x);
      ocr->normal[1] = oc->_N_y /* BILERP(oc->_N_y) (MEM01) */;
      ocr->normal[2] = BILERP(oc->_N_z);
    }

    if (oc->_do_chop) {
      ocr->disp[0] = BILERP(oc->_disp_x);
      ocr->disp[2] = BILERP(oc->_disp_z);
    }
    else {
      ocr->disp[0] = 0.0;
      ocr->disp[2] = 0.0;
    }

    if (oc->_do_jacobian) {
      compute_eigenstuff(ocr, BILERP(oc->_Jxx), BILERP(oc->_Jzz), BILERP(oc->_Jxz));
    }
  }
#  undef BILERP

  BLI_rw_mutex_unlock(&oc->oceanmutex);
}

void BKE_ocean_eval_uv_catrom(struct Ocean *oc, struct OceanResult *ocr, float u, float v)
{
  int i0, i1, i2, i3, j0, j1, j2, j3;
  float frac_x, frac_z;
  float uu, vv;

  /* first wrap the texture so 0 <= (u, v) < 1 */
  u = fmod(u, 1.0f);
  v = fmod(v, 1.0f);

  if (u < 0) {
    u += 1.0f;
  }
  if (v < 0) {
    v += 1.0f;
  }

  BLI_rw_mutex_lock(&oc->oceanmutex, THREAD_LOCK_READ);

  uu = u * oc->_M;
  vv = v * oc->_N;

  i1 = (int)floor(uu);
  j1 = (int)floor(vv);

  i2 = (i1 + 1);
  j2 = (j1 + 1);

  frac_x = uu - i1;
  frac_z = vv - j1;

  i1 = i1 % oc->_M;
  j1 = j1 % oc->_N;

  i2 = i2 % oc->_M;
  j2 = j2 % oc->_N;

  i0 = (i1 - 1);
  i3 = (i2 + 1);
  i0 = i0 < 0 ? i0 + oc->_M : i0;
  i3 = i3 >= oc->_M ? i3 - oc->_M : i3;

  j0 = (j1 - 1);
  j3 = (j2 + 1);
  j0 = j0 < 0 ? j0 + oc->_N : j0;
  j3 = j3 >= oc->_N ? j3 - oc->_N : j3;

#  define INTERP(m) \
    catrom(catrom(m[i0 * oc->_N + j0], \
                  m[i1 * oc->_N + j0], \
                  m[i2 * oc->_N + j0], \
                  m[i3 * oc->_N + j0], \
                  frac_x), \
           catrom(m[i0 * oc->_N + j1], \
                  m[i1 * oc->_N + j1], \
                  m[i2 * oc->_N + j1], \
                  m[i3 * oc->_N + j1], \
                  frac_x), \
           catrom(m[i0 * oc->_N + j2], \
                  m[i1 * oc->_N + j2], \
                  m[i2 * oc->_N + j2], \
                  m[i3 * oc->_N + j2], \
                  frac_x), \
           catrom(m[i0 * oc->_N + j3], \
                  m[i1 * oc->_N + j3], \
                  m[i2 * oc->_N + j3], \
                  m[i3 * oc->_N + j3], \
                  frac_x), \
           frac_z)

  {
    if (oc->_do_disp_y) {
      ocr->disp[1] = INTERP(oc->_disp_y);
    }
    if (oc->_do_normals) {
      ocr->normal[0] = INTERP(oc->_N_x);
      ocr->normal[1] = oc->_N_y /* INTERP(oc->_N_y) (MEM01) */;
      ocr->normal[2] = INTERP(oc->_N_z);
    }
    if (oc->_do_chop) {
      ocr->disp[0] = INTERP(oc->_disp_x);
      ocr->disp[2] = INTERP(oc->_disp_z);
    }
    else {
      ocr->disp[0] = 0.0;
      ocr->disp[2] = 0.0;
    }

    if (oc->_do_jacobian) {
      compute_eigenstuff(ocr, INTERP(oc->_Jxx), INTERP(oc->_Jzz), INTERP(oc->_Jxz));
    }
  }
#  undef INTERP

  BLI_rw_mutex_unlock(&oc->oceanmutex);
}

void BKE_ocean_eval_xz(struct Ocean *oc, struct OceanResult *ocr, float x, float z)
{
  BKE_ocean_eval_uv(oc, ocr, x / oc->_Lx, z / oc->_Lz);
}

void BKE_ocean_eval_xz_catrom(struct Ocean *oc, struct OceanResult *ocr, float x, float z)
{
  BKE_ocean_eval_uv_catrom(oc, ocr, x / oc->_Lx, z / oc->_Lz);
}

void BKE_ocean_eval_ij(struct Ocean *oc, struct OceanResult *ocr, int i, int j)
{
  BLI_rw_mutex_lock(&oc->oceanmutex, THREAD_LOCK_READ);

  i = abs(i) % oc->_M;
  j = abs(j) % oc->_N;

  ocr->disp[1] = oc->_do_disp_y ? (float)oc->_disp_y[i * oc->_N + j] : 0.0f;

  if (oc->_do_chop) {
    ocr->disp[0] = oc->_disp_x[i * oc->_N + j];
    ocr->disp[2] = oc->_disp_z[i * oc->_N + j];
  }
  else {
    ocr->disp[0] = 0.0f;
    ocr->disp[2] = 0.0f;
  }

  if (oc->_do_normals) {
    ocr->normal[0] = oc->_N_x[i * oc->_N + j];
    ocr->normal[1] = oc->_N_y /* oc->_N_y[i * oc->_N + j] (MEM01) */;
    ocr->normal[2] = oc->_N_z[i * oc->_N + j];

    normalize_v3(ocr->normal);
  }

  if (oc->_do_jacobian) {
    compute_eigenstuff(
        ocr, oc->_Jxx[i * oc->_N + j], oc->_Jzz[i * oc->_N + j], oc->_Jxz[i * oc->_N + j]);
  }

  BLI_rw_mutex_unlock(&oc->oceanmutex);
}

typedef struct OceanSimulateData {
  Ocean *o;
  float t;
  float scale;
  float chop_amount;
} OceanSimulateData;

static void ocean_compute_htilda(void *__restrict userdata,
                                 const int i,
                                 const TaskParallelTLS *__restrict UNUSED(tls))
{
  OceanSimulateData *osd = userdata;
  const Ocean *o = osd->o;
  const float scale = osd->scale;
  const float t = osd->t;

  int j;

  /* Note the <= _N/2 here, see the FFTW documentation
   * about the mechanics of the complex->real fft storage. */
  for (j = 0; j <= o->_N / 2; j++) {
    fftw_complex exp_param1;
    fftw_complex exp_param2;
    fftw_complex conj_param;

    init_complex(exp_param1, 0.0, omega(o->_k[i * (1 + o->_N / 2) + j], o->_depth) * t);
    init_complex(exp_param2, 0.0, -omega(o->_k[i * (1 + o->_N / 2) + j], o->_depth) * t);
    exp_complex(exp_param1, exp_param1);
    exp_complex(exp_param2, exp_param2);
    conj_complex(conj_param, o->_h0_minus[i * o->_N + j]);

    mul_complex_c(exp_param1, o->_h0[i * o->_N + j], exp_param1);
    mul_complex_c(exp_param2, conj_param, exp_param2);

    add_comlex_c(o->_htilda[i * (1 + o->_N / 2) + j], exp_param1, exp_param2);
    mul_complex_f(o->_fft_in[i * (1 + o->_N / 2) + j], o->_htilda[i * (1 + o->_N / 2) + j], scale);
  }
}

static void ocean_compute_displacement_y(TaskPool *__restrict pool, void *UNUSED(taskdata))
{
  OceanSimulateData *osd = BLI_task_pool_user_data(pool);
  const Ocean *o = osd->o;

  fftw_execute(o->_disp_y_plan);
}

static void ocean_compute_displacement_x(TaskPool *__restrict pool, void *UNUSED(taskdata))
{
  OceanSimulateData *osd = BLI_task_pool_user_data(pool);
  const Ocean *o = osd->o;
  const float scale = osd->scale;
  const float chop_amount = osd->chop_amount;
  int i, j;

  for (i = 0; i < o->_M; i++) {
    for (j = 0; j <= o->_N / 2; j++) {
      fftw_complex mul_param;
      fftw_complex minus_i;

      init_complex(minus_i, 0.0, -1.0);
      init_complex(mul_param, -scale, 0);
      mul_complex_f(mul_param, mul_param, chop_amount);
      mul_complex_c(mul_param, mul_param, minus_i);
      mul_complex_c(mul_param, mul_param, o->_htilda[i * (1 + o->_N / 2) + j]);
      mul_complex_f(mul_param,
                    mul_param,
                    ((o->_k[i * (1 + o->_N / 2) + j] == 0.0f) ?
                         0.0f :
                         o->_kx[i] / o->_k[i * (1 + o->_N / 2) + j]));
      init_complex(o->_fft_in_x[i * (1 + o->_N / 2) + j], real_c(mul_param), image_c(mul_param));
    }
  }
  fftw_execute(o->_disp_x_plan);
}

static void ocean_compute_displacement_z(TaskPool *__restrict pool, void *UNUSED(taskdata))
{
  OceanSimulateData *osd = BLI_task_pool_user_data(pool);
  const Ocean *o = osd->o;
  const float scale = osd->scale;
  const float chop_amount = osd->chop_amount;
  int i, j;

  for (i = 0; i < o->_M; i++) {
    for (j = 0; j <= o->_N / 2; j++) {
      fftw_complex mul_param;
      fftw_complex minus_i;

      init_complex(minus_i, 0.0, -1.0);
      init_complex(mul_param, -scale, 0);
      mul_complex_f(mul_param, mul_param, chop_amount);
      mul_complex_c(mul_param, mul_param, minus_i);
      mul_complex_c(mul_param, mul_param, o->_htilda[i * (1 + o->_N / 2) + j]);
      mul_complex_f(mul_param,
                    mul_param,
                    ((o->_k[i * (1 + o->_N / 2) + j] == 0.0f) ?
                         0.0f :
                         o->_kz[j] / o->_k[i * (1 + o->_N / 2) + j]));
      init_complex(o->_fft_in_z[i * (1 + o->_N / 2) + j], real_c(mul_param), image_c(mul_param));
    }
  }
  fftw_execute(o->_disp_z_plan);
}

static void ocean_compute_jacobian_jxx(TaskPool *__restrict pool, void *UNUSED(taskdata))
{
  OceanSimulateData *osd = BLI_task_pool_user_data(pool);
  const Ocean *o = osd->o;
  const float chop_amount = osd->chop_amount;
  int i, j;

  for (i = 0; i < o->_M; i++) {
    for (j = 0; j <= o->_N / 2; j++) {
      fftw_complex mul_param;

      // init_complex(mul_param, -scale, 0);
      init_complex(mul_param, -1, 0);

      mul_complex_f(mul_param, mul_param, chop_amount);
      mul_complex_c(mul_param, mul_param, o->_htilda[i * (1 + o->_N / 2) + j]);
      mul_complex_f(mul_param,
                    mul_param,
                    ((o->_k[i * (1 + o->_N / 2) + j] == 0.0f) ?
                         0.0f :
                         o->_kx[i] * o->_kx[i] / o->_k[i * (1 + o->_N / 2) + j]));
      init_complex(o->_fft_in_jxx[i * (1 + o->_N / 2) + j], real_c(mul_param), image_c(mul_param));
    }
  }
  fftw_execute(o->_Jxx_plan);

  for (i = 0; i < o->_M; i++) {
    for (j = 0; j < o->_N; j++) {
      o->_Jxx[i * o->_N + j] += 1.0;
    }
  }
}

static void ocean_compute_jacobian_jzz(TaskPool *__restrict pool, void *UNUSED(taskdata))
{
  OceanSimulateData *osd = BLI_task_pool_user_data(pool);
  const Ocean *o = osd->o;
  const float chop_amount = osd->chop_amount;
  int i, j;

  for (i = 0; i < o->_M; i++) {
    for (j = 0; j <= o->_N / 2; j++) {
      fftw_complex mul_param;

      // init_complex(mul_param, -scale, 0);
      init_complex(mul_param, -1, 0);

      mul_complex_f(mul_param, mul_param, chop_amount);
      mul_complex_c(mul_param, mul_param, o->_htilda[i * (1 + o->_N / 2) + j]);
      mul_complex_f(mul_param,
                    mul_param,
                    ((o->_k[i * (1 + o->_N / 2) + j] == 0.0f) ?
                         0.0f :
                         o->_kz[j] * o->_kz[j] / o->_k[i * (1 + o->_N / 2) + j]));
      init_complex(o->_fft_in_jzz[i * (1 + o->_N / 2) + j], real_c(mul_param), image_c(mul_param));
    }
  }
  fftw_execute(o->_Jzz_plan);

  for (i = 0; i < o->_M; i++) {
    for (j = 0; j < o->_N; j++) {
      o->_Jzz[i * o->_N + j] += 1.0;
    }
  }
}

static void ocean_compute_jacobian_jxz(TaskPool *__restrict pool, void *UNUSED(taskdata))
{
  OceanSimulateData *osd = BLI_task_pool_user_data(pool);
  const Ocean *o = osd->o;
  const float chop_amount = osd->chop_amount;
  int i, j;

  for (i = 0; i < o->_M; i++) {
    for (j = 0; j <= o->_N / 2; j++) {
      fftw_complex mul_param;

      // init_complex(mul_param, -scale, 0);
      init_complex(mul_param, -1, 0);

      mul_complex_f(mul_param, mul_param, chop_amount);
      mul_complex_c(mul_param, mul_param, o->_htilda[i * (1 + o->_N / 2) + j]);
      mul_complex_f(mul_param,
                    mul_param,
                    ((o->_k[i * (1 + o->_N / 2) + j] == 0.0f) ?
                         0.0f :
                         o->_kx[i] * o->_kz[j] / o->_k[i * (1 + o->_N / 2) + j]));
      init_complex(o->_fft_in_jxz[i * (1 + o->_N / 2) + j], real_c(mul_param), image_c(mul_param));
    }
  }
  fftw_execute(o->_Jxz_plan);
}

static void ocean_compute_normal_x(TaskPool *__restrict pool, void *UNUSED(taskdata))
{
  OceanSimulateData *osd = BLI_task_pool_user_data(pool);
  const Ocean *o = osd->o;
  int i, j;

  for (i = 0; i < o->_M; i++) {
    for (j = 0; j <= o->_N / 2; j++) {
      fftw_complex mul_param;

      init_complex(mul_param, 0.0, -1.0);
      mul_complex_c(mul_param, mul_param, o->_htilda[i * (1 + o->_N / 2) + j]);
      mul_complex_f(mul_param, mul_param, o->_kx[i]);
      init_complex(o->_fft_in_nx[i * (1 + o->_N / 2) + j], real_c(mul_param), image_c(mul_param));
    }
  }
  fftw_execute(o->_N_x_plan);
}

static void ocean_compute_normal_z(TaskPool *__restrict pool, void *UNUSED(taskdata))
{
  OceanSimulateData *osd = BLI_task_pool_user_data(pool);
  const Ocean *o = osd->o;
  int i, j;

  for (i = 0; i < o->_M; i++) {
    for (j = 0; j <= o->_N / 2; j++) {
      fftw_complex mul_param;

      init_complex(mul_param, 0.0, -1.0);
      mul_complex_c(mul_param, mul_param, o->_htilda[i * (1 + o->_N / 2) + j]);
      mul_complex_f(mul_param, mul_param, o->_kz[i]);
      init_complex(o->_fft_in_nz[i * (1 + o->_N / 2) + j], real_c(mul_param), image_c(mul_param));
    }
  }
  fftw_execute(o->_N_z_plan);
}

bool BKE_ocean_is_valid(const struct Ocean *o)
{
  return o->_k != NULL;
}

void BKE_ocean_simulate(struct Ocean *o, float t, float scale, float chop_amount)
{
  TaskPool *pool;

  OceanSimulateData osd;

  scale *= o->normalize_factor;

  osd.o = o;
  osd.t = t;
  osd.scale = scale;
  osd.chop_amount = chop_amount;

  pool = BLI_task_pool_create(&osd, TASK_PRIORITY_HIGH);

  BLI_rw_mutex_lock(&o->oceanmutex, THREAD_LOCK_WRITE);

  /* Note about multi-threading here: we have to run a first set of computations (htilda one)
   * before we can run all others, since they all depend on it.
   * So we make a first parallelized forloop run for htilda,
   * and then pack all other computations into a set of parallel tasks.
   * This is not optimal in all cases,
   * but remains reasonably simple and should be OK most of the time. */

  /* compute a new htilda */
  TaskParallelSettings settings;
  BLI_parallel_range_settings_defaults(&settings);
  settings.use_threading = (o->_M > 16);
  BLI_task_parallel_range(0, o->_M, &osd, ocean_compute_htilda, &settings);

  if (o->_do_disp_y) {
    BLI_task_pool_push(pool, ocean_compute_displacement_y, NULL, false, NULL);
  }

  if (o->_do_chop) {
    BLI_task_pool_push(pool, ocean_compute_displacement_x, NULL, false, NULL);
    BLI_task_pool_push(pool, ocean_compute_displacement_z, NULL, false, NULL);
  }

  if (o->_do_jacobian) {
    BLI_task_pool_push(pool, ocean_compute_jacobian_jxx, NULL, false, NULL);
    BLI_task_pool_push(pool, ocean_compute_jacobian_jzz, NULL, false, NULL);
    BLI_task_pool_push(pool, ocean_compute_jacobian_jxz, NULL, false, NULL);
  }

  if (o->_do_normals) {
    BLI_task_pool_push(pool, ocean_compute_normal_x, NULL, false, NULL);
    BLI_task_pool_push(pool, ocean_compute_normal_z, NULL, false, NULL);
    o->_N_y = 1.0f / scale;
  }

  BLI_task_pool_work_and_wait(pool);

  BLI_rw_mutex_unlock(&o->oceanmutex);

  BLI_task_pool_free(pool);
}

static void set_height_normalize_factor(struct Ocean *oc)
{
  float res = 1.0;
  float max_h = 0.0;

  int i, j;

  if (!oc->_do_disp_y) {
    return;
  }

  oc->normalize_factor = 1.0;

  BKE_ocean_simulate(oc, 0.0, 1.0, 0);

  BLI_rw_mutex_lock(&oc->oceanmutex, THREAD_LOCK_READ);

  for (i = 0; i < oc->_M; i++) {
    for (j = 0; j < oc->_N; j++) {
      if (max_h < fabs(oc->_disp_y[i * oc->_N + j])) {
        max_h = fabs(oc->_disp_y[i * oc->_N + j]);
      }
    }
  }

  BLI_rw_mutex_unlock(&oc->oceanmutex);

  if (max_h == 0.0f) {
    max_h = 0.00001f; /* just in case ... */
  }

  res = 1.0f / (max_h);

  oc->normalize_factor = res;
}

struct Ocean *BKE_ocean_add(void)
{
  Ocean *oc = MEM_callocN(sizeof(Ocean), "ocean sim data");

  BLI_rw_mutex_init(&oc->oceanmutex);

  return oc;
}

bool BKE_ocean_ensure(struct OceanModifierData *omd, const int resolution)
{
  if (omd->ocean) {
    /* Check that the ocean has the same resolution than we want now. */
    if (omd->ocean->_M == resolution * resolution) {
      return false;
    }

    BKE_ocean_free(omd->ocean);
  }

  omd->ocean = BKE_ocean_add();
  BKE_ocean_init_from_modifier(omd->ocean, omd, resolution);
  return true;
}

bool BKE_ocean_init_from_modifier(struct Ocean *ocean,
                                  struct OceanModifierData const *omd,
                                  const int resolution)
{
  short do_heightfield, do_chop, do_normals, do_jacobian, do_spray;

  do_heightfield = true;
  do_chop = (omd->chop_amount > 0);
  do_normals = (omd->flag & MOD_OCEAN_GENERATE_NORMALS);
  do_jacobian = (omd->flag & MOD_OCEAN_GENERATE_FOAM);
  do_spray = do_jacobian && (omd->flag & MOD_OCEAN_GENERATE_SPRAY);

  BKE_ocean_free_data(ocean);

  return BKE_ocean_init(ocean,
                        resolution * resolution,
                        resolution * resolution,
                        omd->spatial_size,
                        omd->spatial_size,
                        omd->wind_velocity,
                        omd->smallest_wave,
                        1.0,
                        omd->wave_direction,
                        omd->damp,
                        omd->wave_alignment,
                        omd->depth,
                        omd->time,
                        omd->spectrum,
                        omd->fetch_jonswap,
                        omd->sharpen_peak_jonswap,
                        do_heightfield,
                        do_chop,
                        do_spray,
                        do_normals,
                        do_jacobian,
                        omd->seed);
}

bool BKE_ocean_init(struct Ocean *o,
                    int M,
                    int N,
                    float Lx,
                    float Lz,
                    float V,
                    float l,
                    float A,
                    float w,
                    float damp,
                    float alignment,
                    float depth,
                    float time,
                    int spectrum,
                    float fetch_jonswap,
                    float sharpen_peak_jonswap,
                    short do_height_field,
                    short do_chop,
                    short do_spray,
                    short do_normals,
                    short do_jacobian,
                    int seed)
{
  int i, j, ii;

  BLI_rw_mutex_lock(&o->oceanmutex, THREAD_LOCK_WRITE);

  o->_M = M;
  o->_N = N;
  o->_V = V;
  o->_l = l;
  o->_A = A;
  o->_w = w;
  o->_damp_reflections = 1.0f - damp;
  o->_wind_alignment = alignment * 10.0f;
  o->_depth = depth;
  o->_Lx = Lx;
  o->_Lz = Lz;
  o->_wx = cos(w);
  o->_wz = -sin(w);        /* wave direction */
  o->_L = V * V / GRAVITY; /* largest wave for a given velocity V */
  o->time = time;

  /* Spectrum to use. */
  o->_spectrum = spectrum;

  /* Common JONSWAP parameters. */
  o->_fetch_jonswap = fetch_jonswap;
  o->_sharpen_peak_jonswap = sharpen_peak_jonswap * 10.0f;

  /* NOTE: most modifiers don't account for failure to allocate.
   * In this case however a large resolution can easily perform large allocations that fail,
   * support early exiting in this case. */
  if ((o->_k = (float *)MEM_mallocN(sizeof(float) * (size_t)M * (1 + N / 2), "ocean_k")) &&
      (o->_h0 = (fftw_complex *)MEM_mallocN(sizeof(fftw_complex) * (size_t)M * N, "ocean_h0")) &&
      (o->_h0_minus = (fftw_complex *)MEM_mallocN(sizeof(fftw_complex) * (size_t)M * N,
                                                  "ocean_h0_minus")) &&
      (o->_kx = (float *)MEM_mallocN(sizeof(float) * o->_M, "ocean_kx")) &&
      (o->_kz = (float *)MEM_mallocN(sizeof(float) * o->_N, "ocean_kz"))) {
    /* Success. */
  }
  else {
    MEM_SAFE_FREE(o->_k);
    MEM_SAFE_FREE(o->_h0);
    MEM_SAFE_FREE(o->_h0_minus);
    MEM_SAFE_FREE(o->_kx);
    MEM_SAFE_FREE(o->_kz);

    BLI_rw_mutex_unlock(&o->oceanmutex);
    return false;
  }

  o->_do_disp_y = do_height_field;
  o->_do_normals = do_normals;
  o->_do_spray = do_spray;
  o->_do_chop = do_chop;
  o->_do_jacobian = do_jacobian;

  /* make this robust in the face of erroneous usage */
  if (o->_Lx == 0.0f) {
    o->_Lx = 0.001f;
  }

  if (o->_Lz == 0.0f) {
    o->_Lz = 0.001f;
  }

  /* the +ve components and DC */
  for (i = 0; i <= o->_M / 2; i++) {
    o->_kx[i] = 2.0f * (float)M_PI * i / o->_Lx;
  }

  /* the -ve components */
  for (i = o->_M - 1, ii = 0; i > o->_M / 2; i--, ii++) {
    o->_kx[i] = -2.0f * (float)M_PI * ii / o->_Lx;
  }

  /* the +ve components and DC */
  for (i = 0; i <= o->_N / 2; i++) {
    o->_kz[i] = 2.0f * (float)M_PI * i / o->_Lz;
  }

  /* the -ve components */
  for (i = o->_N - 1, ii = 0; i > o->_N / 2; i--, ii++) {
    o->_kz[i] = -2.0f * (float)M_PI * ii / o->_Lz;
  }

  /* pre-calculate the k matrix */
  for (i = 0; i < o->_M; i++) {
    for (j = 0; j <= o->_N / 2; j++) {
      o->_k[(size_t)i * (1 + o->_N / 2) + j] = sqrt(o->_kx[i] * o->_kx[i] + o->_kz[j] * o->_kz[j]);
    }
  }

  RNG *rng = BLI_rng_new(seed);

  for (i = 0; i < o->_M; i++) {
    for (j = 0; j < o->_N; j++) {
      /* This ensures we get a value tied to the surface location, avoiding dramatic surface
       * change with changing resolution.
       * Explicitly cast to signed int first to ensure consistent behavior on all processors,
       * since behavior of float to unsigned int cast is undefined in C. */
      const int hash_x = o->_kx[i] * 360.0f;
      const int hash_z = o->_kz[j] * 360.0f;
      int new_seed = seed + BLI_hash_int_2d(hash_x, hash_z);

      BLI_rng_seed(rng, new_seed);
      float r1 = gaussRand(rng);
      float r2 = gaussRand(rng);

      fftw_complex r1r2;
      init_complex(r1r2, r1, r2);
      switch (o->_spectrum) {
        case MOD_OCEAN_SPECTRUM_JONSWAP:
          mul_complex_f(o->_h0[i * o->_N + j],
                        r1r2,
                        (float)(sqrt(BLI_ocean_spectrum_jonswap(o, o->_kx[i], o->_kz[j]) / 2.0f)));
          mul_complex_f(
              o->_h0_minus[i * o->_N + j],
              r1r2,
              (float)(sqrt(BLI_ocean_spectrum_jonswap(o, -o->_kx[i], -o->_kz[j]) / 2.0f)));
          break;
        case MOD_OCEAN_SPECTRUM_TEXEL_MARSEN_ARSLOE:
          mul_complex_f(
              o->_h0[i * o->_N + j],
              r1r2,
              (float)(sqrt(BLI_ocean_spectrum_texelmarsenarsloe(o, o->_kx[i], o->_kz[j]) / 2.0f)));
          mul_complex_f(
              o->_h0_minus[i * o->_N + j],
              r1r2,
              (float)(sqrt(BLI_ocean_spectrum_texelmarsenarsloe(o, -o->_kx[i], -o->_kz[j]) /
                           2.0f)));
          break;
        case MOD_OCEAN_SPECTRUM_PIERSON_MOSKOWITZ:
          mul_complex_f(
              o->_h0[i * o->_N + j],
              r1r2,
              (float)(sqrt(BLI_ocean_spectrum_piersonmoskowitz(o, o->_kx[i], o->_kz[j]) / 2.0f)));
          mul_complex_f(
              o->_h0_minus[i * o->_N + j],
              r1r2,
              (float)(sqrt(BLI_ocean_spectrum_piersonmoskowitz(o, -o->_kx[i], -o->_kz[j]) /
                           2.0f)));
          break;
        default:
          mul_complex_f(
              o->_h0[i * o->_N + j], r1r2, (float)(sqrt(Ph(o, o->_kx[i], o->_kz[j]) / 2.0f)));
          mul_complex_f(o->_h0_minus[i * o->_N + j],
                        r1r2,
                        (float)(sqrt(Ph(o, -o->_kx[i], -o->_kz[j]) / 2.0f)));
          break;
      }
    }
  }

  o->_fft_in = (fftw_complex *)MEM_mallocN(o->_M * (1 + o->_N / 2) * sizeof(fftw_complex),
                                           "ocean_fft_in");
  o->_htilda = (fftw_complex *)MEM_mallocN(o->_M * (1 + o->_N / 2) * sizeof(fftw_complex),
                                           "ocean_htilda");

  BLI_thread_lock(LOCK_FFTW);

  if (o->_do_disp_y) {
    o->_disp_y = (double *)MEM_mallocN(o->_M * o->_N * sizeof(double), "ocean_disp_y");
    o->_disp_y_plan = fftw_plan_dft_c2r_2d(o->_M, o->_N, o->_fft_in, o->_disp_y, FFTW_ESTIMATE);
  }

  if (o->_do_normals) {
    o->_fft_in_nx = (fftw_complex *)MEM_mallocN(o->_M * (1 + o->_N / 2) * sizeof(fftw_complex),
                                                "ocean_fft_in_nx");
    o->_fft_in_nz = (fftw_complex *)MEM_mallocN(o->_M * (1 + o->_N / 2) * sizeof(fftw_complex),
                                                "ocean_fft_in_nz");

    o->_N_x = (double *)MEM_mallocN(o->_M * o->_N * sizeof(double), "ocean_N_x");
    // o->_N_y = (float *) fftwf_malloc(o->_M * o->_N * sizeof(float)); /* (MEM01) */
    o->_N_z = (double *)MEM_mallocN(o->_M * o->_N * sizeof(double), "ocean_N_z");

    o->_N_x_plan = fftw_plan_dft_c2r_2d(o->_M, o->_N, o->_fft_in_nx, o->_N_x, FFTW_ESTIMATE);
    o->_N_z_plan = fftw_plan_dft_c2r_2d(o->_M, o->_N, o->_fft_in_nz, o->_N_z, FFTW_ESTIMATE);
  }

  if (o->_do_chop) {
    o->_fft_in_x = (fftw_complex *)MEM_mallocN(o->_M * (1 + o->_N / 2) * sizeof(fftw_complex),
                                               "ocean_fft_in_x");
    o->_fft_in_z = (fftw_complex *)MEM_mallocN(o->_M * (1 + o->_N / 2) * sizeof(fftw_complex),
                                               "ocean_fft_in_z");

    o->_disp_x = (double *)MEM_mallocN(o->_M * o->_N * sizeof(double), "ocean_disp_x");
    o->_disp_z = (double *)MEM_mallocN(o->_M * o->_N * sizeof(double), "ocean_disp_z");

    o->_disp_x_plan = fftw_plan_dft_c2r_2d(o->_M, o->_N, o->_fft_in_x, o->_disp_x, FFTW_ESTIMATE);
    o->_disp_z_plan = fftw_plan_dft_c2r_2d(o->_M, o->_N, o->_fft_in_z, o->_disp_z, FFTW_ESTIMATE);
  }
  if (o->_do_jacobian) {
    o->_fft_in_jxx = (fftw_complex *)MEM_mallocN(o->_M * (1 + o->_N / 2) * sizeof(fftw_complex),
                                                 "ocean_fft_in_jxx");
    o->_fft_in_jzz = (fftw_complex *)MEM_mallocN(o->_M * (1 + o->_N / 2) * sizeof(fftw_complex),
                                                 "ocean_fft_in_jzz");
    o->_fft_in_jxz = (fftw_complex *)MEM_mallocN(o->_M * (1 + o->_N / 2) * sizeof(fftw_complex),
                                                 "ocean_fft_in_jxz");

    o->_Jxx = (double *)MEM_mallocN(o->_M * o->_N * sizeof(double), "ocean_Jxx");
    o->_Jzz = (double *)MEM_mallocN(o->_M * o->_N * sizeof(double), "ocean_Jzz");
    o->_Jxz = (double *)MEM_mallocN(o->_M * o->_N * sizeof(double), "ocean_Jxz");

    o->_Jxx_plan = fftw_plan_dft_c2r_2d(o->_M, o->_N, o->_fft_in_jxx, o->_Jxx, FFTW_ESTIMATE);
    o->_Jzz_plan = fftw_plan_dft_c2r_2d(o->_M, o->_N, o->_fft_in_jzz, o->_Jzz, FFTW_ESTIMATE);
    o->_Jxz_plan = fftw_plan_dft_c2r_2d(o->_M, o->_N, o->_fft_in_jxz, o->_Jxz, FFTW_ESTIMATE);
  }

  BLI_thread_unlock(LOCK_FFTW);

  BLI_rw_mutex_unlock(&o->oceanmutex);

  set_height_normalize_factor(o);

  BLI_rng_free(rng);

  return true;
}

void BKE_ocean_free_data(struct Ocean *oc)
{
  if (!oc) {
    return;
  }

  BLI_rw_mutex_lock(&oc->oceanmutex, THREAD_LOCK_WRITE);

  BLI_thread_lock(LOCK_FFTW);

  if (oc->_do_disp_y) {
    fftw_destroy_plan(oc->_disp_y_plan);
    MEM_freeN(oc->_disp_y);
  }

  if (oc->_do_normals) {
    MEM_freeN(oc->_fft_in_nx);
    MEM_freeN(oc->_fft_in_nz);
    fftw_destroy_plan(oc->_N_x_plan);
    fftw_destroy_plan(oc->_N_z_plan);
    MEM_freeN(oc->_N_x);
    // fftwf_free(oc->_N_y); /* (MEM01) */
    MEM_freeN(oc->_N_z);
  }

  if (oc->_do_chop) {
    MEM_freeN(oc->_fft_in_x);
    MEM_freeN(oc->_fft_in_z);
    fftw_destroy_plan(oc->_disp_x_plan);
    fftw_destroy_plan(oc->_disp_z_plan);
    MEM_freeN(oc->_disp_x);
    MEM_freeN(oc->_disp_z);
  }

  if (oc->_do_jacobian) {
    MEM_freeN(oc->_fft_in_jxx);
    MEM_freeN(oc->_fft_in_jzz);
    MEM_freeN(oc->_fft_in_jxz);
    fftw_destroy_plan(oc->_Jxx_plan);
    fftw_destroy_plan(oc->_Jzz_plan);
    fftw_destroy_plan(oc->_Jxz_plan);
    MEM_freeN(oc->_Jxx);
    MEM_freeN(oc->_Jzz);
    MEM_freeN(oc->_Jxz);
  }

  BLI_thread_unlock(LOCK_FFTW);

  if (oc->_fft_in) {
    MEM_freeN(oc->_fft_in);
  }

  /* check that ocean data has been initialized */
  if (oc->_htilda) {
    MEM_freeN(oc->_htilda);
    MEM_freeN(oc->_k);
    MEM_freeN(oc->_h0);
    MEM_freeN(oc->_h0_minus);
    MEM_freeN(oc->_kx);
    MEM_freeN(oc->_kz);
  }

  BLI_rw_mutex_unlock(&oc->oceanmutex);
}

void BKE_ocean_free(struct Ocean *oc)
{
  if (!oc) {
    return;
  }

  BKE_ocean_free_data(oc);
  BLI_rw_mutex_end(&oc->oceanmutex);

  MEM_freeN(oc);
}

#  undef GRAVITY

/* ********* Baking/Caching ********* */

#  define CACHE_TYPE_DISPLACE 1
#  define CACHE_TYPE_FOAM 2
#  define CACHE_TYPE_NORMAL 3
#  define CACHE_TYPE_SPRAY 4
#  define CACHE_TYPE_SPRAY_INVERSE 5

static void cache_filename(
    char *string, const char *path, const char *relbase, int frame, int type)
{
  char cachepath[FILE_MAX];
  const char *fname;

  switch (type) {
    case CACHE_TYPE_FOAM:
      fname = "foam_";
      break;
    case CACHE_TYPE_NORMAL:
      fname = "normal_";
      break;
    case CACHE_TYPE_SPRAY:
      fname = "spray_";
      break;
    case CACHE_TYPE_SPRAY_INVERSE:
      fname = "spray_inverse_";
      break;
    case CACHE_TYPE_DISPLACE:
    default:
      fname = "disp_";
      break;
  }

  BLI_join_dirfile(cachepath, sizeof(cachepath), path, fname);

  BKE_image_path_from_imtype(
      string, cachepath, relbase, frame, R_IMF_IMTYPE_OPENEXR, true, true, "");
}

/* silly functions but useful to inline when the args do a lot of indirections */
MINLINE void rgb_to_rgba_unit_alpha(float r_rgba[4], const float rgb[3])
{
  r_rgba[0] = rgb[0];
  r_rgba[1] = rgb[1];
  r_rgba[2] = rgb[2];
  r_rgba[3] = 1.0f;
}
MINLINE void value_to_rgba_unit_alpha(float r_rgba[4], const float value)
{
  r_rgba[0] = value;
  r_rgba[1] = value;
  r_rgba[2] = value;
  r_rgba[3] = 1.0f;
}

void BKE_ocean_free_cache(struct OceanCache *och)
{
  int i, f = 0;

  if (!och) {
    return;
  }

  if (och->ibufs_disp) {
    for (i = och->start, f = 0; i <= och->end; i++, f++) {
      if (och->ibufs_disp[f]) {
        IMB_freeImBuf(och->ibufs_disp[f]);
      }
    }
    MEM_freeN(och->ibufs_disp);
  }

  if (och->ibufs_foam) {
    for (i = och->start, f = 0; i <= och->end; i++, f++) {
      if (och->ibufs_foam[f]) {
        IMB_freeImBuf(och->ibufs_foam[f]);
      }
    }
    MEM_freeN(och->ibufs_foam);
  }

  if (och->ibufs_spray) {
    for (i = och->start, f = 0; i <= och->end; i++, f++) {
      if (och->ibufs_spray[f]) {
        IMB_freeImBuf(och->ibufs_spray[f]);
      }
    }
    MEM_freeN(och->ibufs_spray);
  }

  if (och->ibufs_spray_inverse) {
    for (i = och->start, f = 0; i <= och->end; i++, f++) {
      if (och->ibufs_spray_inverse[f]) {
        IMB_freeImBuf(och->ibufs_spray_inverse[f]);
      }
    }
    MEM_freeN(och->ibufs_spray_inverse);
  }

  if (och->ibufs_norm) {
    for (i = och->start, f = 0; i <= och->end; i++, f++) {
      if (och->ibufs_norm[f]) {
        IMB_freeImBuf(och->ibufs_norm[f]);
      }
    }
    MEM_freeN(och->ibufs_norm);
  }

  if (och->time) {
    MEM_freeN(och->time);
  }
  MEM_freeN(och);
}

void BKE_ocean_cache_eval_uv(
    struct OceanCache *och, struct OceanResult *ocr, int f, float u, float v)
{
  int res_x = och->resolution_x;
  int res_y = och->resolution_y;
  float result[4];

  u = fmod(u, 1.0);
  v = fmod(v, 1.0);

  if (u < 0) {
    u += 1.0f;
  }
  if (v < 0) {
    v += 1.0f;
  }

  if (och->ibufs_disp[f]) {
    ibuf_sample(och->ibufs_disp[f], u, v, (1.0f / (float)res_x), (1.0f / (float)res_y), result);
    copy_v3_v3(ocr->disp, result);
  }

  if (och->ibufs_foam[f]) {
    ibuf_sample(och->ibufs_foam[f], u, v, (1.0f / (float)res_x), (1.0f / (float)res_y), result);
    ocr->foam = result[0];
  }

  if (och->ibufs_spray[f]) {
    ibuf_sample(och->ibufs_spray[f], u, v, (1.0f / (float)res_x), (1.0f / (float)res_y), result);
    copy_v3_v3(ocr->Eplus, result);
  }

  if (och->ibufs_spray_inverse[f]) {
    ibuf_sample(
        och->ibufs_spray_inverse[f], u, v, (1.0f / (float)res_x), (1.0f / (float)res_y), result);
    copy_v3_v3(ocr->Eminus, result);
  }

  if (och->ibufs_norm[f]) {
    ibuf_sample(och->ibufs_norm[f], u, v, (1.0f / (float)res_x), (1.0f / (float)res_y), result);
    copy_v3_v3(ocr->normal, result);
  }
}

void BKE_ocean_cache_eval_ij(struct OceanCache *och, struct OceanResult *ocr, int f, int i, int j)
{
  const int res_x = och->resolution_x;
  const int res_y = och->resolution_y;

  if (i < 0) {
    i = -i;
  }
  if (j < 0) {
    j = -j;
  }

  i = i % res_x;
  j = j % res_y;

  if (och->ibufs_disp[f]) {
    copy_v3_v3(ocr->disp, &och->ibufs_disp[f]->rect_float[4 * (res_x * j + i)]);
  }

  if (och->ibufs_foam[f]) {
    ocr->foam = och->ibufs_foam[f]->rect_float[4 * (res_x * j + i)];
  }

  if (och->ibufs_spray[f]) {
    copy_v3_v3(ocr->Eplus, &och->ibufs_spray[f]->rect_float[4 * (res_x * j + i)]);
  }

  if (och->ibufs_spray_inverse[f]) {
    copy_v3_v3(ocr->Eminus, &och->ibufs_spray_inverse[f]->rect_float[4 * (res_x * j + i)]);
  }

  if (och->ibufs_norm[f]) {
    copy_v3_v3(ocr->normal, &och->ibufs_norm[f]->rect_float[4 * (res_x * j + i)]);
  }
}

struct OceanCache *BKE_ocean_init_cache(const char *bakepath,
                                        const char *relbase,
                                        int start,
                                        int end,
                                        float wave_scale,
                                        float chop_amount,
                                        float foam_coverage,
                                        float foam_fade,
                                        int resolution)
{
  OceanCache *och = MEM_callocN(sizeof(OceanCache), "ocean cache data");

  och->bakepath = bakepath;
  och->relbase = relbase;

  och->start = start;
  och->end = end;
  och->duration = (end - start) + 1;
  och->wave_scale = wave_scale;
  och->chop_amount = chop_amount;
  och->foam_coverage = foam_coverage;
  och->foam_fade = foam_fade;
  och->resolution_x = resolution * resolution;
  och->resolution_y = resolution * resolution;

  och->ibufs_disp = MEM_callocN(sizeof(ImBuf *) * och->duration,
                                "displacement imbuf pointer array");
  och->ibufs_foam = MEM_callocN(sizeof(ImBuf *) * och->duration, "foam imbuf pointer array");
  och->ibufs_spray = MEM_callocN(sizeof(ImBuf *) * och->duration, "spray imbuf pointer array");
  och->ibufs_spray_inverse = MEM_callocN(sizeof(ImBuf *) * och->duration,
                                         "spray_inverse imbuf pointer array");
  och->ibufs_norm = MEM_callocN(sizeof(ImBuf *) * och->duration, "normal imbuf pointer array");

  och->time = NULL;

  return och;
}

void BKE_ocean_simulate_cache(struct OceanCache *och, int frame)
{
  char string[FILE_MAX];
  int f = frame;

  /* ibufs array is zero based, but filenames are based on frame numbers */
  /* still need to clamp frame numbers to valid range of images on disk though */
  CLAMP(frame, och->start, och->end);
  f = frame - och->start; /* shift to 0 based */

  /* if image is already loaded in mem, return */
  if (och->ibufs_disp[f] != NULL) {
    return;
  }

  /* Use default color spaces since we know for sure cache
   * files were saved with default settings too. */

  cache_filename(string, och->bakepath, och->relbase, frame, CACHE_TYPE_DISPLACE);
  och->ibufs_disp[f] = IMB_loadiffname(string, 0, NULL);

  cache_filename(string, och->bakepath, och->relbase, frame, CACHE_TYPE_FOAM);
  och->ibufs_foam[f] = IMB_loadiffname(string, 0, NULL);

  cache_filename(string, och->bakepath, och->relbase, frame, CACHE_TYPE_SPRAY);
  och->ibufs_spray[f] = IMB_loadiffname(string, 0, NULL);

  cache_filename(string, och->bakepath, och->relbase, frame, CACHE_TYPE_SPRAY_INVERSE);
  och->ibufs_spray_inverse[f] = IMB_loadiffname(string, 0, NULL);

  cache_filename(string, och->bakepath, och->relbase, frame, CACHE_TYPE_NORMAL);
  och->ibufs_norm[f] = IMB_loadiffname(string, 0, NULL);
}

void BKE_ocean_bake(struct Ocean *o,
                    struct OceanCache *och,
                    void (*update_cb)(void *, float progress, int *cancel),
                    void *update_cb_data)
{
  /* NOTE(@campbellbarton): some of these values remain uninitialized unless certain options
   * are enabled, take care that #BKE_ocean_eval_ij() initializes a member before use. */
  OceanResult ocr;

  ImageFormatData imf = {0};

  int f, i = 0, x, y, cancel = 0;
  float progress;

  ImBuf *ibuf_foam, *ibuf_disp, *ibuf_normal, *ibuf_spray, *ibuf_spray_inverse;
  float *prev_foam;
  int res_x = och->resolution_x;
  int res_y = och->resolution_y;
  char string[FILE_MAX];
  // RNG *rng;

  if (!o) {
    return;
  }

  if (o->_do_jacobian) {
    prev_foam = MEM_callocN(res_x * res_y * sizeof(float), "previous frame foam bake data");
  }
  else {
    prev_foam = NULL;
  }

  // rng = BLI_rng_new(0);

  /* setup image format */
  imf.imtype = R_IMF_IMTYPE_OPENEXR;
  imf.depth = R_IMF_CHAN_DEPTH_16;
  imf.exr_codec = R_IMF_EXR_CODEC_ZIP;

  for (f = och->start, i = 0; f <= och->end; f++, i++) {

    /* create a new imbuf to store image for this frame */
    ibuf_foam = IMB_allocImBuf(res_x, res_y, 32, IB_rectfloat);
    ibuf_disp = IMB_allocImBuf(res_x, res_y, 32, IB_rectfloat);
    ibuf_normal = IMB_allocImBuf(res_x, res_y, 32, IB_rectfloat);
    ibuf_spray = IMB_allocImBuf(res_x, res_y, 32, IB_rectfloat);
    ibuf_spray_inverse = IMB_allocImBuf(res_x, res_y, 32, IB_rectfloat);

    BKE_ocean_simulate(o, och->time[i], och->wave_scale, och->chop_amount);

    /* add new foam */
    for (y = 0; y < res_y; y++) {
      for (x = 0; x < res_x; x++) {

        BKE_ocean_eval_ij(o, &ocr, x, y);

        /* add to the image */
        rgb_to_rgba_unit_alpha(&ibuf_disp->rect_float[4 * (res_x * y + x)], ocr.disp);

        if (o->_do_jacobian) {
          /* TODO(@campbellbarton): cleanup unused code. */

          float /* r, */ /* UNUSED */ pr = 0.0f, foam_result;
          float neg_disp, neg_eplus;

          ocr.foam = BKE_ocean_jminus_to_foam(ocr.Jminus, och->foam_coverage);

          /* accumulate previous value for this cell */
          if (i > 0) {
            pr = prev_foam[res_x * y + x];
          }

          // r = BLI_rng_get_float(rng); /* UNUSED */ /* randomly reduce foam */

          // pr = pr * och->foam_fade; /* overall fade */

          /* Remember ocean coord sys is Y up!
           * break up the foam where height (Y) is low (wave valley),
           * and X and Z displacement is greatest. */

          neg_disp = ocr.disp[1] < 0.0f ? 1.0f + ocr.disp[1] : 1.0f;
          neg_disp = neg_disp < 0.0f ? 0.0f : neg_disp;

          /* foam, 'ocr.Eplus' only initialized with do_jacobian */
          neg_eplus = ocr.Eplus[2] < 0.0f ? 1.0f + ocr.Eplus[2] : 1.0f;
          neg_eplus = neg_eplus < 0.0f ? 0.0f : neg_eplus;

          if (pr < 1.0f) {
            pr *= pr;
          }

          pr *= och->foam_fade * (0.75f + neg_eplus * 0.25f);

          /* A full clamping should not be needed! */
          foam_result = min_ff(pr + ocr.foam, 1.0f);

          prev_foam[res_x * y + x] = foam_result;

          // foam_result = min_ff(foam_result, 1.0f);

          value_to_rgba_unit_alpha(&ibuf_foam->rect_float[4 * (res_x * y + x)], foam_result);

          /* spray map baking */
          if (o->_do_spray) {
            rgb_to_rgba_unit_alpha(&ibuf_spray->rect_float[4 * (res_x * y + x)], ocr.Eplus);
            rgb_to_rgba_unit_alpha(&ibuf_spray_inverse->rect_float[4 * (res_x * y + x)],
                                   ocr.Eminus);
          }
        }

        if (o->_do_normals) {
          rgb_to_rgba_unit_alpha(&ibuf_normal->rect_float[4 * (res_x * y + x)], ocr.normal);
        }
      }
    }

    /* write the images */
    cache_filename(string, och->bakepath, och->relbase, f, CACHE_TYPE_DISPLACE);
    if (0 == BKE_imbuf_write(ibuf_disp, string, &imf)) {
      printf("Cannot save Displacement File Output to %s\n", string);
    }

    if (o->_do_jacobian) {
      cache_filename(string, och->bakepath, och->relbase, f, CACHE_TYPE_FOAM);
      if (0 == BKE_imbuf_write(ibuf_foam, string, &imf)) {
        printf("Cannot save Foam File Output to %s\n", string);
      }

      if (o->_do_spray) {
        cache_filename(string, och->bakepath, och->relbase, f, CACHE_TYPE_SPRAY);
        if (0 == BKE_imbuf_write(ibuf_spray, string, &imf)) {
          printf("Cannot save Spray File Output to %s\n", string);
        }

        cache_filename(string, och->bakepath, och->relbase, f, CACHE_TYPE_SPRAY_INVERSE);
        if (0 == BKE_imbuf_write(ibuf_spray_inverse, string, &imf)) {
          printf("Cannot save Spray Inverse File Output to %s\n", string);
        }
      }
    }

    if (o->_do_normals) {
      cache_filename(string, och->bakepath, och->relbase, f, CACHE_TYPE_NORMAL);
      if (0 == BKE_imbuf_write(ibuf_normal, string, &imf)) {
        printf("Cannot save Normal File Output to %s\n", string);
      }
    }

    IMB_freeImBuf(ibuf_disp);
    IMB_freeImBuf(ibuf_foam);
    IMB_freeImBuf(ibuf_normal);
    IMB_freeImBuf(ibuf_spray);
    IMB_freeImBuf(ibuf_spray_inverse);

    progress = (f - och->start) / (float)och->duration;

    update_cb(update_cb_data, progress, &cancel);

    if (cancel) {
      if (prev_foam) {
        MEM_freeN(prev_foam);
      }
      // BLI_rng_free(rng);
      return;
    }
  }

  // BLI_rng_free(rng);
  if (prev_foam) {
    MEM_freeN(prev_foam);
  }
  och->baked = 1;
}

#else /* WITH_OCEANSIM */

float BKE_ocean_jminus_to_foam(float UNUSED(jminus), float UNUSED(coverage))
{
  return 0.0f;
}

void BKE_ocean_eval_uv(struct Ocean *UNUSED(oc),
                       struct OceanResult *UNUSED(ocr),
                       float UNUSED(u),
                       float UNUSED(v))
{
}

/* use catmullrom interpolation rather than linear */
void BKE_ocean_eval_uv_catrom(struct Ocean *UNUSED(oc),
                              struct OceanResult *UNUSED(ocr),
                              float UNUSED(u),
                              float UNUSED(v))
{
}

void BKE_ocean_eval_xz(struct Ocean *UNUSED(oc),
                       struct OceanResult *UNUSED(ocr),
                       float UNUSED(x),
                       float UNUSED(z))
{
}

void BKE_ocean_eval_xz_catrom(struct Ocean *UNUSED(oc),
                              struct OceanResult *UNUSED(ocr),
                              float UNUSED(x),
                              float UNUSED(z))
{
}

void BKE_ocean_eval_ij(struct Ocean *UNUSED(oc),
                       struct OceanResult *UNUSED(ocr),
                       int UNUSED(i),
                       int UNUSED(j))
{
}

void BKE_ocean_simulate(struct Ocean *UNUSED(o),
                        float UNUSED(t),
                        float UNUSED(scale),
                        float UNUSED(chop_amount))
{
}

struct Ocean *BKE_ocean_add(void)
{
  Ocean *oc = MEM_callocN(sizeof(Ocean), "ocean sim data");

  return oc;
}

bool BKE_ocean_init(struct Ocean *UNUSED(o),
                    int UNUSED(M),
                    int UNUSED(N),
                    float UNUSED(Lx),
                    float UNUSED(Lz),
                    float UNUSED(V),
                    float UNUSED(l),
                    float UNUSED(A),
                    float UNUSED(w),
                    float UNUSED(damp),
                    float UNUSED(alignment),
                    float UNUSED(depth),
                    float UNUSED(time),
                    int UNUSED(spectrum),
                    float UNUSED(fetch_jonswap),
                    float UNUSED(sharpen_peak_jonswap),
                    short UNUSED(do_height_field),
                    short UNUSED(do_chop),
                    short UNUSED(do_spray),
                    short UNUSED(do_normals),
                    short UNUSED(do_jacobian),
                    int UNUSED(seed))
{
  return false;
}

void BKE_ocean_free_data(struct Ocean *UNUSED(oc))
{
}

void BKE_ocean_free(struct Ocean *oc)
{
  if (!oc) {
    return;
  }
  MEM_freeN(oc);
}

/* ********* Baking/Caching ********* */

void BKE_ocean_free_cache(struct OceanCache *och)
{
  if (!och) {
    return;
  }

  MEM_freeN(och);
}

void BKE_ocean_cache_eval_uv(struct OceanCache *UNUSED(och),
                             struct OceanResult *UNUSED(ocr),
                             int UNUSED(f),
                             float UNUSED(u),
                             float UNUSED(v))
{
}

void BKE_ocean_cache_eval_ij(struct OceanCache *UNUSED(och),
                             struct OceanResult *UNUSED(ocr),
                             int UNUSED(f),
                             int UNUSED(i),
                             int UNUSED(j))
{
}

OceanCache *BKE_ocean_init_cache(const char *UNUSED(bakepath),
                                 const char *UNUSED(relbase),
                                 int UNUSED(start),
                                 int UNUSED(end),
                                 float UNUSED(wave_scale),
                                 float UNUSED(chop_amount),
                                 float UNUSED(foam_coverage),
                                 float UNUSED(foam_fade),
                                 int UNUSED(resolution))
{
  OceanCache *och = MEM_callocN(sizeof(OceanCache), "ocean cache data");

  return och;
}

void BKE_ocean_simulate_cache(struct OceanCache *UNUSED(och), int UNUSED(frame))
{
}

void BKE_ocean_bake(struct Ocean *UNUSED(o),
                    struct OceanCache *UNUSED(och),
                    void (*update_cb)(void *, float progress, int *cancel),
                    void *UNUSED(update_cb_data))
{
  /* unused */
  (void)update_cb;
}

bool BKE_ocean_init_from_modifier(struct Ocean *UNUSED(ocean),
                                  struct OceanModifierData const *UNUSED(omd),
                                  int UNUSED(resolution))
{
  return true;
}

#endif /* WITH_OCEANSIM */

void BKE_ocean_free_modifier_cache(struct OceanModifierData *omd)
{
  BKE_ocean_free_cache(omd->oceancache);
  omd->oceancache = NULL;
  omd->cached = false;
}
