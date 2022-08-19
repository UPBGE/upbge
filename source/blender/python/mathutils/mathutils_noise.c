/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup mathutils
 *
 * This file defines the 'noise' module, a general purpose module to access
 * blenders noise functions.
 */

/************************/
/* Blender Noise Module */
/************************/

#include <Python.h>

#include "BLI_math.h"
#include "BLI_noise.h"
#include "BLI_utildefines.h"

#include "DNA_texture_types.h"

#include "../generic/py_capi_utils.h"

#include "mathutils.h"
#include "mathutils_noise.h"

/*-----------------------------------------*/
/* 'mersenne twister' random number generator */

/* A C-program for MT19937, with initialization improved 2002/2/10.
 * Coded by Takuji Nishimura and Makoto Matsumoto.
 * This is a faster version by taking Shawn Cokus's optimization,
 * Matthe Bellew's simplification, Isaku Wada's real version.
 *
 * Before using, initialize the state by using
 * `init_genrand(seed)` or `init_by_array(init_key, key_length)`.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright 1997-2002 Makoto Matsumoto and Takuji Nishimura, All rights reserved.
 *
 * Any feedback is very welcome.
 * http://www.math.sci.hiroshima-u.ac.jp/~m-mat/MT/emt.html
 * email: m-mat @ math.sci.hiroshima-u.ac.jp (remove space). */

/* Period parameters */
#define N 624
#define M 397
#define MATRIX_A 0x9908b0dfUL /* constant vector a */
#define UMASK 0x80000000UL    /* most significant w-r bits */
#define LMASK 0x7fffffffUL    /* least significant r bits */
#define MIXBITS(u, v) (((u)&UMASK) | ((v)&LMASK))
#define TWIST(u, v) ((MIXBITS(u, v) >> 1) ^ ((v)&1UL ? MATRIX_A : 0UL))

static ulong state[N]; /* The array for the state vector. */
static int left = 1;
static int initf = 0;
static ulong *next;
static float state_offset_vector[3 * 3];

/* initializes state[N] with a seed */
static void init_genrand(ulong s)
{
  int j;
  state[0] = s & 0xffffffffUL;
  for (j = 1; j < N; j++) {
    state[j] = (1812433253UL * (state[j - 1] ^ (state[j - 1] >> 30)) + j);
    /* See Knuth TAOCP Vol2. 3rd Ed. P.106 for multiplier.
     * In the previous versions, MSBs of the seed affect
     * only MSBs of the array state[].
     * 2002/01/09 modified by Makoto Matsumoto. */
    state[j] &= 0xffffffffUL; /* for >32 bit machines */
  }
  left = 1;
  initf = 1;

  /* update vector offset */
  {
    const ulong *state_offset = &state[N - ARRAY_SIZE(state_offset_vector)];
    const float range = 32; /* range in both pos/neg direction */
    for (j = 0; j < ARRAY_SIZE(state_offset_vector); j++, state_offset++) {
      /* overflow is fine here */
      state_offset_vector[j] = (float)(int)(*state_offset) * (1.0f / ((float)INT_MAX / range));
    }
  }
}

static void next_state(void)
{
  ulong *p = state;
  int j;

  /* If init_genrand() has not been called, a default initial seed is used. */
  if (initf == 0) {
    init_genrand(5489UL);
  }

  left = N;
  next = state;

  for (j = N - M + 1; --j; p++) {
    *p = p[M] ^ TWIST(p[0], p[1]);
  }

  for (j = M; --j; p++) {
    *p = p[M - N] ^ TWIST(p[0], p[1]);
  }

  *p = p[M - N] ^ TWIST(p[0], state[0]);
}

/*------------------------------------------------------------*/

static void setRndSeed(int seed)
{
  if (seed == 0) {
    init_genrand(time(NULL));
  }
  else {
    init_genrand(seed);
  }
}

/* float number in range [0, 1) using the mersenne twister rng */
static float frand(void)
{
  ulong y;

  if (--left == 0) {
    next_state();
  }
  y = *next++;

  /* Tempering */
  y ^= (y >> 11);
  y ^= (y << 7) & 0x9d2c5680UL;
  y ^= (y << 15) & 0xefc60000UL;
  y ^= (y >> 18);

  return (float)y / 4294967296.0f;
}

/*------------------------------------------------------------*/
/* Utility Functions */
/*------------------------------------------------------------*/

#define BPY_NOISE_BASIS_ENUM_DOC \
  "   :arg noise_basis: Enumerator in ['BLENDER', 'PERLIN_ORIGINAL', 'PERLIN_NEW', " \
  "'VORONOI_F1', 'VORONOI_F2', " \
  "'VORONOI_F3', 'VORONOI_F4', 'VORONOI_F2F1', 'VORONOI_CRACKLE', " \
  "'CELLNOISE'].\n" \
  "   :type noise_basis: string\n"

#define BPY_NOISE_METRIC_ENUM_DOC \
  "   :arg distance_metric: Enumerator in ['DISTANCE', 'DISTANCE_SQUARED', 'MANHATTAN', " \
  "'CHEBYCHEV', " \
  "'MINKOVSKY', 'MINKOVSKY_HALF', 'MINKOVSKY_FOUR'].\n" \
  "   :type distance_metric: string\n"

/* Noise basis enum */
#define DEFAULT_NOISE_TYPE TEX_STDPERLIN

static PyC_FlagSet bpy_noise_types[] = {
    {TEX_BLENDER, "BLENDER"},
    {TEX_STDPERLIN, "PERLIN_ORIGINAL"},
    {TEX_NEWPERLIN, "PERLIN_NEW"},
    {TEX_VORONOI_F1, "VORONOI_F1"},
    {TEX_VORONOI_F2, "VORONOI_F2"},
    {TEX_VORONOI_F3, "VORONOI_F3"},
    {TEX_VORONOI_F4, "VORONOI_F4"},
    {TEX_VORONOI_F2F1, "VORONOI_F2F1"},
    {TEX_VORONOI_CRACKLE, "VORONOI_CRACKLE"},
    {TEX_CELLNOISE, "CELLNOISE"},
    {0, NULL},
};

/* Metric basis enum */
#define DEFAULT_METRIC_TYPE TEX_DISTANCE

static PyC_FlagSet bpy_noise_metrics[] = {
    {TEX_DISTANCE, "DISTANCE"},
    {TEX_DISTANCE_SQUARED, "DISTANCE_SQUARED"},
    {TEX_MANHATTAN, "MANHATTAN"},
    {TEX_CHEBYCHEV, "CHEBYCHEV"},
    {TEX_MINKOVSKY, "MINKOVSKY"},
    {TEX_MINKOVSKY_HALF, "MINKOVSKY_HALF"},
    {TEX_MINKOVSKY_FOUR, "MINKOVSKY_FOUR"},
    {0, NULL},
};

/* Fills an array of length size with random numbers in the range (-1, 1). */
static void rand_vn(float *array_tar, const int size)
{
  float *array_pt = array_tar + (size - 1);
  int i = size;
  while (i--) {
    *(array_pt--) = 2.0f * frand() - 1.0f;
  }
}

/* Fills an array of length 3 with noise values */
static void noise_vector(float x, float y, float z, int nb, float v[3])
{
  /* Simply evaluate noise at 3 different positions */
  const float *ofs = state_offset_vector;
  for (int j = 0; j < 3; j++) {
    v[j] = (2.0f * BLI_noise_generic_noise(1.0f, x + ofs[0], y + ofs[1], z + ofs[2], false, nb) -
            1.0f);
    ofs += 3;
  }
}

/* Returns a turbulence value for a given position (x, y, z) */
static float turb(
    float x, float y, float z, int oct, int hard, int nb, float ampscale, float freqscale)
{
  float amp, out, t;
  int i;
  amp = 1.0f;
  out = (float)(2.0f * BLI_noise_generic_noise(1.0f, x, y, z, false, nb) - 1.0f);
  if (hard) {
    out = fabsf(out);
  }
  for (i = 1; i < oct; i++) {
    amp *= ampscale;
    x *= freqscale;
    y *= freqscale;
    z *= freqscale;
    t = (float)(amp * (2.0f * BLI_noise_generic_noise(1.0f, x, y, z, false, nb) - 1.0f));
    if (hard) {
      t = fabsf(t);
    }
    out += t;
  }
  return out;
}

/* Fills an array of length 3 with the turbulence vector for a given
 * position (x, y, z) */
static void vTurb(float x,
                  float y,
                  float z,
                  int oct,
                  int hard,
                  int nb,
                  float ampscale,
                  float freqscale,
                  float v[3])
{
  float amp, t[3];
  int i;
  amp = 1.0f;
  noise_vector(x, y, z, nb, v);
  if (hard) {
    v[0] = fabsf(v[0]);
    v[1] = fabsf(v[1]);
    v[2] = fabsf(v[2]);
  }
  for (i = 1; i < oct; i++) {
    amp *= ampscale;
    x *= freqscale;
    y *= freqscale;
    z *= freqscale;
    noise_vector(x, y, z, nb, t);
    if (hard) {
      t[0] = fabsf(t[0]);
      t[1] = fabsf(t[1]);
      t[2] = fabsf(t[2]);
    }
    v[0] += amp * t[0];
    v[1] += amp * t[1];
    v[2] += amp * t[2];
  }
}

/*-------------------------DOC STRINGS ---------------------------*/
PyDoc_STRVAR(M_Noise_doc, "The Blender noise module");

/*------------------------------------------------------------*/
/* Python Functions */
/*------------------------------------------------------------*/

PyDoc_STRVAR(M_Noise_random_doc,
             ".. function:: random()\n"
             "\n"
             "   Returns a random number in the range [0, 1).\n"
             "\n"
             "   :return: The random number.\n"
             "   :rtype: float\n");
static PyObject *M_Noise_random(PyObject *UNUSED(self))
{
  return PyFloat_FromDouble(frand());
}

PyDoc_STRVAR(M_Noise_random_unit_vector_doc,
             ".. function:: random_unit_vector(size=3)\n"
             "\n"
             "   Returns a unit vector with random entries.\n"
             "\n"
             "   :arg size: The size of the vector to be produced, in the range [2, 4].\n"
             "   :type size: int\n"
             "   :return: The random unit vector.\n"
             "   :rtype: :class:`mathutils.Vector`\n");
static PyObject *M_Noise_random_unit_vector(PyObject *UNUSED(self), PyObject *args, PyObject *kw)
{
  static const char *kwlist[] = {"size", NULL};
  float vec[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  float norm = 2.0f;
  int vec_num = 3;

  if (!PyArg_ParseTupleAndKeywords(
          args, kw, "|$i:random_unit_vector", (char **)kwlist, &vec_num)) {
    return NULL;
  }

  if (vec_num > 4 || vec_num < 2) {
    PyErr_SetString(PyExc_ValueError, "Vector(): invalid size");
    return NULL;
  }

  while (norm == 0.0f || norm > 1.0f) {
    rand_vn(vec, vec_num);
    norm = normalize_vn(vec, vec_num);
  }

  return Vector_CreatePyObject(vec, vec_num, NULL);
}

PyDoc_STRVAR(M_Noise_random_vector_doc,
             ".. function:: random_vector(size=3)\n"
             "\n"
             "   Returns a vector with random entries in the range (-1, 1).\n"
             "\n"
             "   :arg size: The size of the vector to be produced.\n"
             "   :type size: int\n"
             "   :return: The random vector.\n"
             "   :rtype: :class:`mathutils.Vector`\n");
static PyObject *M_Noise_random_vector(PyObject *UNUSED(self), PyObject *args, PyObject *kw)
{
  static const char *kwlist[] = {"size", NULL};
  float *vec = NULL;
  int vec_num = 3;

  if (!PyArg_ParseTupleAndKeywords(args, kw, "|$i:random_vector", (char **)kwlist, &vec_num)) {
    return NULL;
  }

  if (vec_num < 2) {
    PyErr_SetString(PyExc_ValueError, "Vector(): invalid size");
    return NULL;
  }

  vec = PyMem_New(float, vec_num);

  rand_vn(vec, vec_num);

  return Vector_CreatePyObject_alloc(vec, vec_num, NULL);
}

PyDoc_STRVAR(M_Noise_seed_set_doc,
             ".. function:: seed_set(seed)\n"
             "\n"
             "   Sets the random seed used for random_unit_vector, and random.\n"
             "\n"
             "   :arg seed: Seed used for the random generator.\n"
             "      When seed is zero, the current time will be used instead.\n"
             "   :type seed: int\n");
static PyObject *M_Noise_seed_set(PyObject *UNUSED(self), PyObject *args)
{
  int s;
  if (!PyArg_ParseTuple(args, "i:seed_set", &s)) {
    return NULL;
  }
  setRndSeed(s);
  Py_RETURN_NONE;
}

PyDoc_STRVAR(M_Noise_noise_doc,
             ".. function:: noise(position, noise_basis='PERLIN_ORIGINAL')\n"
             "\n"
             "   Returns noise value from the noise basis at the position specified.\n"
             "\n"
             "   :arg position: The position to evaluate the selected noise function.\n"
             "   :type position: :class:`mathutils.Vector`\n" BPY_NOISE_BASIS_ENUM_DOC
             "   :return: The noise value.\n"
             "   :rtype: float\n");
static PyObject *M_Noise_noise(PyObject *UNUSED(self), PyObject *args, PyObject *kw)
{
  static const char *kwlist[] = {"", "noise_basis", NULL};
  PyObject *value;
  float vec[3];
  const char *noise_basis_str = NULL;
  int noise_basis_enum = DEFAULT_NOISE_TYPE;

  if (!PyArg_ParseTupleAndKeywords(
          args, kw, "O|$s:noise", (char **)kwlist, &value, &noise_basis_str)) {
    return NULL;
  }

  if (!noise_basis_str) {
    /* pass through */
  }
  else if (PyC_FlagSet_ValueFromID(bpy_noise_types, noise_basis_str, &noise_basis_enum, "noise") ==
           -1) {
    return NULL;
  }

  if (mathutils_array_parse(vec, 3, 3, value, "noise: invalid 'position' arg") == -1) {
    return NULL;
  }

  return PyFloat_FromDouble(
      (2.0f * BLI_noise_generic_noise(1.0f, vec[0], vec[1], vec[2], false, noise_basis_enum) -
       1.0f));
}

PyDoc_STRVAR(M_Noise_noise_vector_doc,
             ".. function:: noise_vector(position, noise_basis='PERLIN_ORIGINAL')\n"
             "\n"
             "   Returns the noise vector from the noise basis at the specified position.\n"
             "\n"
             "   :arg position: The position to evaluate the selected noise function.\n"
             "   :type position: :class:`mathutils.Vector`\n" BPY_NOISE_BASIS_ENUM_DOC
             "   :return: The noise vector.\n"
             "   :rtype: :class:`mathutils.Vector`\n");
static PyObject *M_Noise_noise_vector(PyObject *UNUSED(self), PyObject *args, PyObject *kw)
{
  static const char *kwlist[] = {"", "noise_basis", NULL};
  PyObject *value;
  float vec[3], r_vec[3];
  const char *noise_basis_str = NULL;
  int noise_basis_enum = DEFAULT_NOISE_TYPE;

  if (!PyArg_ParseTupleAndKeywords(
          args, kw, "O|$s:noise_vector", (char **)kwlist, &value, &noise_basis_str)) {
    return NULL;
  }

  if (!noise_basis_str) {
    /* pass through */
  }
  else if (PyC_FlagSet_ValueFromID(
               bpy_noise_types, noise_basis_str, &noise_basis_enum, "noise_vector") == -1) {
    return NULL;
  }

  if (mathutils_array_parse(vec, 3, 3, value, "noise_vector: invalid 'position' arg") == -1) {
    return NULL;
  }

  noise_vector(vec[0], vec[1], vec[2], noise_basis_enum, r_vec);

  return Vector_CreatePyObject(r_vec, 3, NULL);
}

PyDoc_STRVAR(M_Noise_turbulence_doc,
             ".. function:: turbulence(position, octaves, hard, noise_basis='PERLIN_ORIGINAL', "
             "amplitude_scale=0.5, frequency_scale=2.0)\n"
             "\n"
             "   Returns the turbulence value from the noise basis at the specified position.\n"
             "\n"
             "   :arg position: The position to evaluate the selected noise function.\n"
             "   :type position: :class:`mathutils.Vector`\n"
             "   :arg octaves: The number of different noise frequencies used.\n"
             "   :type octaves: int\n"
             "   :arg hard: Specifies whether returned turbulence is hard (sharp transitions) or "
             "soft (smooth transitions).\n"
             "   :type hard: boolean\n" BPY_NOISE_BASIS_ENUM_DOC
             "   :arg amplitude_scale: The amplitude scaling factor.\n"
             "   :type amplitude_scale: float\n"
             "   :arg frequency_scale: The frequency scaling factor\n"
             "   :type frequency_scale: float\n"
             "   :return: The turbulence value.\n"
             "   :rtype: float\n");
static PyObject *M_Noise_turbulence(PyObject *UNUSED(self), PyObject *args, PyObject *kw)
{
  static const char *kwlist[] = {
      "", "", "", "noise_basis", "amplitude_scale", "frequency_scale", NULL};
  PyObject *value;
  float vec[3];
  const char *noise_basis_str = NULL;
  int oct, hd, noise_basis_enum = DEFAULT_NOISE_TYPE;
  float as = 0.5f, fs = 2.0f;

  if (!PyArg_ParseTupleAndKeywords(args,
                                   kw,
                                   "Oii|$sff:turbulence",
                                   (char **)kwlist,
                                   &value,
                                   &oct,
                                   &hd,
                                   &noise_basis_str,
                                   &as,
                                   &fs)) {
    return NULL;
  }

  if (!noise_basis_str) {
    /* pass through */
  }
  else if (PyC_FlagSet_ValueFromID(
               bpy_noise_types, noise_basis_str, &noise_basis_enum, "turbulence") == -1) {
    return NULL;
  }

  if (mathutils_array_parse(vec, 3, 3, value, "turbulence: invalid 'position' arg") == -1) {
    return NULL;
  }

  return PyFloat_FromDouble(turb(vec[0], vec[1], vec[2], oct, hd, noise_basis_enum, as, fs));
}

PyDoc_STRVAR(M_Noise_turbulence_vector_doc,
             ".. function:: turbulence_vector(position, octaves, hard, "
             "noise_basis='PERLIN_ORIGINAL', amplitude_scale=0.5, frequency_scale=2.0)\n"
             "\n"
             "   Returns the turbulence vector from the noise basis at the specified position.\n"
             "\n"
             "   :arg position: The position to evaluate the selected noise function.\n"
             "   :type position: :class:`mathutils.Vector`\n"
             "   :arg octaves: The number of different noise frequencies used.\n"
             "   :type octaves: int\n"
             "   :arg hard: Specifies whether returned turbulence is hard (sharp transitions) or "
             "soft (smooth transitions).\n"
             "   :type hard: boolean\n" BPY_NOISE_BASIS_ENUM_DOC
             "   :arg amplitude_scale: The amplitude scaling factor.\n"
             "   :type amplitude_scale: float\n"
             "   :arg frequency_scale: The frequency scaling factor\n"
             "   :type frequency_scale: float\n"
             "   :return: The turbulence vector.\n"
             "   :rtype: :class:`mathutils.Vector`\n");
static PyObject *M_Noise_turbulence_vector(PyObject *UNUSED(self), PyObject *args, PyObject *kw)
{
  static const char *kwlist[] = {
      "", "", "", "noise_basis", "amplitude_scale", "frequency_scale", NULL};
  PyObject *value;
  float vec[3], r_vec[3];
  const char *noise_basis_str = NULL;
  int oct, hd, noise_basis_enum = DEFAULT_NOISE_TYPE;
  float as = 0.5f, fs = 2.0f;

  if (!PyArg_ParseTupleAndKeywords(args,
                                   kw,
                                   "Oii|$sff:turbulence_vector",
                                   (char **)kwlist,
                                   &value,
                                   &oct,
                                   &hd,
                                   &noise_basis_str,
                                   &as,
                                   &fs)) {
    return NULL;
  }

  if (!noise_basis_str) {
    /* pass through */
  }
  else if (PyC_FlagSet_ValueFromID(
               bpy_noise_types, noise_basis_str, &noise_basis_enum, "turbulence_vector") == -1) {
    return NULL;
  }

  if (mathutils_array_parse(vec, 3, 3, value, "turbulence_vector: invalid 'position' arg") == -1) {
    return NULL;
  }

  vTurb(vec[0], vec[1], vec[2], oct, hd, noise_basis_enum, as, fs, r_vec);

  return Vector_CreatePyObject(r_vec, 3, NULL);
}

/* F. Kenton Musgrave's fractal functions */
PyDoc_STRVAR(
    M_Noise_fractal_doc,
    ".. function:: fractal(position, H, lacunarity, octaves, noise_basis='PERLIN_ORIGINAL')\n"
    "\n"
    "   Returns the fractal Brownian motion (fBm) noise value from the noise basis at the "
    "specified position.\n"
    "\n"
    "   :arg position: The position to evaluate the selected noise function.\n"
    "   :type position: :class:`mathutils.Vector`\n"
    "   :arg H: The fractal increment factor.\n"
    "   :type H: float\n"
    "   :arg lacunarity: The gap between successive frequencies.\n"
    "   :type lacunarity: float\n"
    "   :arg octaves: The number of different noise frequencies used.\n"
    "   :type octaves: int\n" BPY_NOISE_BASIS_ENUM_DOC
    "   :return: The fractal Brownian motion noise value.\n"
    "   :rtype: float\n");
static PyObject *M_Noise_fractal(PyObject *UNUSED(self), PyObject *args, PyObject *kw)
{
  static const char *kwlist[] = {"", "", "", "", "noise_basis", NULL};
  PyObject *value;
  float vec[3];
  const char *noise_basis_str = NULL;
  float H, lac, oct;
  int noise_basis_enum = DEFAULT_NOISE_TYPE;

  if (!PyArg_ParseTupleAndKeywords(args,
                                   kw,
                                   "Offf|$s:fractal",
                                   (char **)kwlist,
                                   &value,
                                   &H,
                                   &lac,
                                   &oct,
                                   &noise_basis_str)) {
    return NULL;
  }

  if (!noise_basis_str) {
    /* pass through */
  }
  else if (PyC_FlagSet_ValueFromID(
               bpy_noise_types, noise_basis_str, &noise_basis_enum, "fractal") == -1) {
    return NULL;
  }

  if (mathutils_array_parse(vec, 3, 3, value, "fractal: invalid 'position' arg") == -1) {
    return NULL;
  }

  return PyFloat_FromDouble(
      BLI_noise_mg_fbm(vec[0], vec[1], vec[2], H, lac, oct, noise_basis_enum));
}

PyDoc_STRVAR(
    M_Noise_multi_fractal_doc,
    ".. function:: multi_fractal(position, H, lacunarity, octaves, "
    "noise_basis='PERLIN_ORIGINAL')\n"
    "\n"
    "   Returns multifractal noise value from the noise basis at the specified position.\n"
    "\n"
    "   :arg position: The position to evaluate the selected noise function.\n"
    "   :type position: :class:`mathutils.Vector`\n"
    "   :arg H: The fractal increment factor.\n"
    "   :type H: float\n"
    "   :arg lacunarity: The gap between successive frequencies.\n"
    "   :type lacunarity: float\n"
    "   :arg octaves: The number of different noise frequencies used.\n"
    "   :type octaves: int\n" BPY_NOISE_BASIS_ENUM_DOC
    "   :return: The multifractal noise value.\n"
    "   :rtype: float\n");
static PyObject *M_Noise_multi_fractal(PyObject *UNUSED(self), PyObject *args, PyObject *kw)
{
  static const char *kwlist[] = {"", "", "", "", "noise_basis", NULL};
  PyObject *value;
  float vec[3];
  const char *noise_basis_str = NULL;
  float H, lac, oct;
  int noise_basis_enum = DEFAULT_NOISE_TYPE;

  if (!PyArg_ParseTupleAndKeywords(args,
                                   kw,
                                   "Offf|$s:multi_fractal",
                                   (char **)kwlist,
                                   &value,
                                   &H,
                                   &lac,
                                   &oct,
                                   &noise_basis_str)) {
    return NULL;
  }

  if (!noise_basis_str) {
    /* pass through */
  }
  else if (PyC_FlagSet_ValueFromID(
               bpy_noise_types, noise_basis_str, &noise_basis_enum, "multi_fractal") == -1) {
    return NULL;
  }

  if (mathutils_array_parse(vec, 3, 3, value, "multi_fractal: invalid 'position' arg") == -1) {
    return NULL;
  }

  return PyFloat_FromDouble(
      BLI_noise_mg_multi_fractal(vec[0], vec[1], vec[2], H, lac, oct, noise_basis_enum));
}

PyDoc_STRVAR(M_Noise_variable_lacunarity_doc,
             ".. function:: variable_lacunarity(position, distortion, "
             "noise_type1='PERLIN_ORIGINAL', noise_type2='PERLIN_ORIGINAL')\n"
             "\n"
             "   Returns variable lacunarity noise value, a distorted variety of noise, from "
             "noise type 1 distorted by noise type 2 at the specified position.\n"
             "\n"
             "   :arg position: The position to evaluate the selected noise function.\n"
             "   :type position: :class:`mathutils.Vector`\n"
             "   :arg distortion: The amount of distortion.\n"
             "   :type distortion: float\n"
             "   :arg noise_type1: Enumerator in ['BLENDER', 'PERLIN_ORIGINAL', 'PERLIN_NEW', "
             "'VORONOI_F1', 'VORONOI_F2', "
             "'VORONOI_F3', 'VORONOI_F4', 'VORONOI_F2F1', 'VORONOI_CRACKLE', "
             "'CELLNOISE'].\n"
             "   :type noise_type1: string\n"
             "   :arg noise_type2: Enumerator in ['BLENDER', 'PERLIN_ORIGINAL', 'PERLIN_NEW', "
             "'VORONOI_F1', 'VORONOI_F2', "
             "'VORONOI_F3', 'VORONOI_F4', 'VORONOI_F2F1', 'VORONOI_CRACKLE', "
             "'CELLNOISE'].\n"
             "   :type noise_type2: string\n"
             "   :return: The variable lacunarity noise value.\n"
             "   :rtype: float\n");
static PyObject *M_Noise_variable_lacunarity(PyObject *UNUSED(self), PyObject *args, PyObject *kw)
{
  static const char *kwlist[] = {"", "", "noise_type1", "noise_type2", NULL};
  PyObject *value;
  float vec[3];
  const char *noise_type1_str = NULL, *noise_type2_str = NULL;
  float d;
  int noise_type1_enum = DEFAULT_NOISE_TYPE, noise_type2_enum = DEFAULT_NOISE_TYPE;

  if (!PyArg_ParseTupleAndKeywords(args,
                                   kw,
                                   "Of|$ss:variable_lacunarity",
                                   (char **)kwlist,
                                   &value,
                                   &d,
                                   &noise_type1_str,
                                   &noise_type2_str)) {
    return NULL;
  }

  if (!noise_type1_str) {
    /* pass through */
  }
  else if (PyC_FlagSet_ValueFromID(
               bpy_noise_types, noise_type1_str, &noise_type1_enum, "variable_lacunarity") == -1) {
    return NULL;
  }

  if (!noise_type2_str) {
    /* pass through */
  }
  else if (PyC_FlagSet_ValueFromID(
               bpy_noise_types, noise_type2_str, &noise_type2_enum, "variable_lacunarity") == -1) {
    return NULL;
  }

  if (mathutils_array_parse(vec, 3, 3, value, "variable_lacunarity: invalid 'position' arg") ==
      -1) {
    return NULL;
  }

  return PyFloat_FromDouble(BLI_noise_mg_variable_lacunarity(
      vec[0], vec[1], vec[2], d, noise_type1_enum, noise_type2_enum));
}

PyDoc_STRVAR(
    M_Noise_hetero_terrain_doc,
    ".. function:: hetero_terrain(position, H, lacunarity, octaves, offset, "
    "noise_basis='PERLIN_ORIGINAL')\n"
    "\n"
    "   Returns the heterogeneous terrain value from the noise basis at the specified position.\n"
    "\n"
    "   :arg position: The position to evaluate the selected noise function.\n"
    "   :type position: :class:`mathutils.Vector`\n"
    "   :arg H: The fractal dimension of the roughest areas.\n"
    "   :type H: float\n"
    "   :arg lacunarity: The gap between successive frequencies.\n"
    "   :type lacunarity: float\n"
    "   :arg octaves: The number of different noise frequencies used.\n"
    "   :type octaves: int\n"
    "   :arg offset: The height of the terrain above 'sea level'.\n"
    "   :type offset: float\n" BPY_NOISE_BASIS_ENUM_DOC
    "   :return: The heterogeneous terrain value.\n"
    "   :rtype: float\n");
static PyObject *M_Noise_hetero_terrain(PyObject *UNUSED(self), PyObject *args, PyObject *kw)
{
  static const char *kwlist[] = {"", "", "", "", "", "noise_basis", NULL};
  PyObject *value;
  float vec[3];
  const char *noise_basis_str = NULL;
  float H, lac, oct, ofs;
  int noise_basis_enum = DEFAULT_NOISE_TYPE;

  if (!PyArg_ParseTupleAndKeywords(args,
                                   kw,
                                   "Offff|$s:hetero_terrain",
                                   (char **)kwlist,
                                   &value,
                                   &H,
                                   &lac,
                                   &oct,
                                   &ofs,
                                   &noise_basis_str)) {
    return NULL;
  }

  if (!noise_basis_str) {
    /* pass through */
  }
  else if (PyC_FlagSet_ValueFromID(
               bpy_noise_types, noise_basis_str, &noise_basis_enum, "hetero_terrain") == -1) {
    return NULL;
  }

  if (mathutils_array_parse(vec, 3, 3, value, "hetero_terrain: invalid 'position' arg") == -1) {
    return NULL;
  }

  return PyFloat_FromDouble(
      BLI_noise_mg_hetero_terrain(vec[0], vec[1], vec[2], H, lac, oct, ofs, noise_basis_enum));
}

PyDoc_STRVAR(
    M_Noise_hybrid_multi_fractal_doc,
    ".. function:: hybrid_multi_fractal(position, H, lacunarity, octaves, offset, gain, "
    "noise_basis='PERLIN_ORIGINAL')\n"
    "\n"
    "   Returns hybrid multifractal value from the noise basis at the specified position.\n"
    "\n"
    "   :arg position: The position to evaluate the selected noise function.\n"
    "   :type position: :class:`mathutils.Vector`\n"
    "   :arg H: The fractal dimension of the roughest areas.\n"
    "   :type H: float\n"
    "   :arg lacunarity: The gap between successive frequencies.\n"
    "   :type lacunarity: float\n"
    "   :arg octaves: The number of different noise frequencies used.\n"
    "   :type octaves: int\n"
    "   :arg offset: The height of the terrain above 'sea level'.\n"
    "   :type offset: float\n"
    "   :arg gain: Scaling applied to the values.\n"
    "   :type gain: float\n" BPY_NOISE_BASIS_ENUM_DOC
    "   :return: The hybrid multifractal value.\n"
    "   :rtype: float\n");
static PyObject *M_Noise_hybrid_multi_fractal(PyObject *UNUSED(self), PyObject *args, PyObject *kw)
{
  static const char *kwlist[] = {"", "", "", "", "", "", "noise_basis", NULL};
  PyObject *value;
  float vec[3];
  const char *noise_basis_str = NULL;
  float H, lac, oct, ofs, gn;
  int noise_basis_enum = DEFAULT_NOISE_TYPE;

  if (!PyArg_ParseTupleAndKeywords(args,
                                   kw,
                                   "Offfff|$s:hybrid_multi_fractal",
                                   (char **)kwlist,
                                   &value,
                                   &H,
                                   &lac,
                                   &oct,
                                   &ofs,
                                   &gn,
                                   &noise_basis_str)) {
    return NULL;
  }

  if (!noise_basis_str) {
    /* pass through */
  }
  else if (PyC_FlagSet_ValueFromID(
               bpy_noise_types, noise_basis_str, &noise_basis_enum, "hybrid_multi_fractal") ==
           -1) {
    return NULL;
  }

  if (mathutils_array_parse(vec, 3, 3, value, "hybrid_multi_fractal: invalid 'position' arg") ==
      -1) {
    return NULL;
  }

  return PyFloat_FromDouble(BLI_noise_mg_hybrid_multi_fractal(
      vec[0], vec[1], vec[2], H, lac, oct, ofs, gn, noise_basis_enum));
}

PyDoc_STRVAR(
    M_Noise_ridged_multi_fractal_doc,
    ".. function:: ridged_multi_fractal(position, H, lacunarity, octaves, offset, gain, "
    "noise_basis='PERLIN_ORIGINAL')\n"
    "\n"
    "   Returns ridged multifractal value from the noise basis at the specified position.\n"
    "\n"
    "   :arg position: The position to evaluate the selected noise function.\n"
    "   :type position: :class:`mathutils.Vector`\n"
    "   :arg H: The fractal dimension of the roughest areas.\n"
    "   :type H: float\n"
    "   :arg lacunarity: The gap between successive frequencies.\n"
    "   :type lacunarity: float\n"
    "   :arg octaves: The number of different noise frequencies used.\n"
    "   :type octaves: int\n"
    "   :arg offset: The height of the terrain above 'sea level'.\n"
    "   :type offset: float\n"
    "   :arg gain: Scaling applied to the values.\n"
    "   :type gain: float\n" BPY_NOISE_BASIS_ENUM_DOC
    "   :return: The ridged multifractal value.\n"
    "   :rtype: float\n");
static PyObject *M_Noise_ridged_multi_fractal(PyObject *UNUSED(self), PyObject *args, PyObject *kw)
{
  static const char *kwlist[] = {"", "", "", "", "", "", "noise_basis", NULL};
  PyObject *value;
  float vec[3];
  const char *noise_basis_str = NULL;
  float H, lac, oct, ofs, gn;
  int noise_basis_enum = DEFAULT_NOISE_TYPE;

  if (!PyArg_ParseTupleAndKeywords(args,
                                   kw,
                                   "Offfff|$s:ridged_multi_fractal",
                                   (char **)kwlist,
                                   &value,
                                   &H,
                                   &lac,
                                   &oct,
                                   &ofs,
                                   &gn,
                                   &noise_basis_str)) {
    return NULL;
  }

  if (!noise_basis_str) {
    /* pass through */
  }
  else if (PyC_FlagSet_ValueFromID(
               bpy_noise_types, noise_basis_str, &noise_basis_enum, "ridged_multi_fractal") ==
           -1) {
    return NULL;
  }

  if (mathutils_array_parse(vec, 3, 3, value, "ridged_multi_fractal: invalid 'position' arg") ==
      -1) {
    return NULL;
  }

  return PyFloat_FromDouble(BLI_noise_mg_ridged_multi_fractal(
      vec[0], vec[1], vec[2], H, lac, oct, ofs, gn, noise_basis_enum));
}

PyDoc_STRVAR(M_Noise_voronoi_doc,
             ".. function:: voronoi(position, distance_metric='DISTANCE', exponent=2.5)\n"
             "\n"
             "   Returns a list of distances to the four closest features and their locations.\n"
             "\n"
             "   :arg position: The position to evaluate the selected noise function.\n"
             "   :type position: :class:`mathutils.Vector`\n" BPY_NOISE_METRIC_ENUM_DOC
             "   :arg exponent: The exponent for Minkowski distance metric.\n"
             "   :type exponent: float\n"
             "   :return: A list of distances to the four closest features and their locations.\n"
             "   :rtype: list of four floats, list of four :class:`mathutils.Vector` types\n");
static PyObject *M_Noise_voronoi(PyObject *UNUSED(self), PyObject *args, PyObject *kw)
{
  static const char *kwlist[] = {"", "distance_metric", "exponent", NULL};
  PyObject *value;
  PyObject *list;
  PyObject *ret;
  float vec[3];
  const char *metric_str = NULL;
  float da[4], pa[12];
  int metric_enum = DEFAULT_METRIC_TYPE;
  float me = 2.5f; /* default minkowski exponent */

  int i;

  if (!PyArg_ParseTupleAndKeywords(
          args, kw, "O|$sf:voronoi", (char **)kwlist, &value, &metric_str, &me)) {
    return NULL;
  }

  if (!metric_str) {
    /* pass through */
  }
  else if (PyC_FlagSet_ValueFromID(bpy_noise_metrics, metric_str, &metric_enum, "voronoi") == -1) {
    return NULL;
  }

  if (mathutils_array_parse(vec, 3, 3, value, "voronoi: invalid 'position' arg") == -1) {
    return NULL;
  }

  list = PyList_New(4);

  BLI_noise_voronoi(vec[0], vec[1], vec[2], da, pa, me, metric_enum);

  for (i = 0; i < 4; i++) {
    PyObject *v = Vector_CreatePyObject(pa + 3 * i, 3, NULL);
    PyList_SET_ITEM(list, i, v);
  }

  ret = Py_BuildValue("[[ffff]O]", da[0], da[1], da[2], da[3], list);
  Py_DECREF(list);
  return ret;
}

PyDoc_STRVAR(M_Noise_cell_doc,
             ".. function:: cell(position)\n"
             "\n"
             "   Returns cell noise value at the specified position.\n"
             "\n"
             "   :arg position: The position to evaluate the selected noise function.\n"
             "   :type position: :class:`mathutils.Vector`\n"
             "   :return: The cell noise value.\n"
             "   :rtype: float\n");
static PyObject *M_Noise_cell(PyObject *UNUSED(self), PyObject *args)
{
  PyObject *value;
  float vec[3];

  if (!PyArg_ParseTuple(args, "O:cell", &value)) {
    return NULL;
  }

  if (mathutils_array_parse(vec, 3, 3, value, "cell: invalid 'position' arg") == -1) {
    return NULL;
  }

  return PyFloat_FromDouble(BLI_noise_cell(vec[0], vec[1], vec[2]));
}

PyDoc_STRVAR(M_Noise_cell_vector_doc,
             ".. function:: cell_vector(position)\n"
             "\n"
             "   Returns cell noise vector at the specified position.\n"
             "\n"
             "   :arg position: The position to evaluate the selected noise function.\n"
             "   :type position: :class:`mathutils.Vector`\n"
             "   :return: The cell noise vector.\n"
             "   :rtype: :class:`mathutils.Vector`\n");
static PyObject *M_Noise_cell_vector(PyObject *UNUSED(self), PyObject *args)
{
  PyObject *value;
  float vec[3], r_vec[3];

  if (!PyArg_ParseTuple(args, "O:cell_vector", &value)) {
    return NULL;
  }

  if (mathutils_array_parse(vec, 3, 3, value, "cell_vector: invalid 'position' arg") == -1) {
    return NULL;
  }

  BLI_noise_cell_v3(vec[0], vec[1], vec[2], r_vec);
  return Vector_CreatePyObject(r_vec, 3, NULL);
}

static PyMethodDef M_Noise_methods[] = {
    {"seed_set", (PyCFunction)M_Noise_seed_set, METH_VARARGS, M_Noise_seed_set_doc},
    {"random", (PyCFunction)M_Noise_random, METH_NOARGS, M_Noise_random_doc},
    {"random_unit_vector",
     (PyCFunction)M_Noise_random_unit_vector,
     METH_VARARGS | METH_KEYWORDS,
     M_Noise_random_unit_vector_doc},
    {"random_vector",
     (PyCFunction)M_Noise_random_vector,
     METH_VARARGS | METH_KEYWORDS,
     M_Noise_random_vector_doc},
    {"noise", (PyCFunction)M_Noise_noise, METH_VARARGS | METH_KEYWORDS, M_Noise_noise_doc},
    {"noise_vector",
     (PyCFunction)M_Noise_noise_vector,
     METH_VARARGS | METH_KEYWORDS,
     M_Noise_noise_vector_doc},
    {"turbulence",
     (PyCFunction)M_Noise_turbulence,
     METH_VARARGS | METH_KEYWORDS,
     M_Noise_turbulence_doc},
    {"turbulence_vector",
     (PyCFunction)M_Noise_turbulence_vector,
     METH_VARARGS | METH_KEYWORDS,
     M_Noise_turbulence_vector_doc},
    {"fractal", (PyCFunction)M_Noise_fractal, METH_VARARGS | METH_KEYWORDS, M_Noise_fractal_doc},
    {"multi_fractal",
     (PyCFunction)M_Noise_multi_fractal,
     METH_VARARGS | METH_KEYWORDS,
     M_Noise_multi_fractal_doc},
    {"variable_lacunarity",
     (PyCFunction)M_Noise_variable_lacunarity,
     METH_VARARGS | METH_KEYWORDS,
     M_Noise_variable_lacunarity_doc},
    {"hetero_terrain",
     (PyCFunction)M_Noise_hetero_terrain,
     METH_VARARGS | METH_KEYWORDS,
     M_Noise_hetero_terrain_doc},
    {"hybrid_multi_fractal",
     (PyCFunction)M_Noise_hybrid_multi_fractal,
     METH_VARARGS | METH_KEYWORDS,
     M_Noise_hybrid_multi_fractal_doc},
    {"ridged_multi_fractal",
     (PyCFunction)M_Noise_ridged_multi_fractal,
     METH_VARARGS | METH_KEYWORDS,
     M_Noise_ridged_multi_fractal_doc},
    {"voronoi", (PyCFunction)M_Noise_voronoi, METH_VARARGS | METH_KEYWORDS, M_Noise_voronoi_doc},
    {"cell", (PyCFunction)M_Noise_cell, METH_VARARGS, M_Noise_cell_doc},
    {"cell_vector", (PyCFunction)M_Noise_cell_vector, METH_VARARGS, M_Noise_cell_vector_doc},
    {NULL, NULL, 0, NULL},
};

static struct PyModuleDef M_Noise_module_def = {
    PyModuleDef_HEAD_INIT,
    "mathutils.noise", /* m_name */
    M_Noise_doc,       /* m_doc */
    0,                 /* m_size */
    M_Noise_methods,   /* m_methods */
    NULL,              /* m_reload */
    NULL,              /* m_traverse */
    NULL,              /* m_clear */
    NULL,              /* m_free */
};

/*----------------------------MODULE INIT-------------------------*/
PyMODINIT_FUNC PyInit_mathutils_noise(void)
{
  PyObject *submodule = PyModule_Create(&M_Noise_module_def);

  /* use current time as seed for random number generator by default */
  setRndSeed(0);

  return submodule;
}
