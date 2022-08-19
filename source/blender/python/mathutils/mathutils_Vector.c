/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup pymathutils
 */

#include <Python.h>

#include "mathutils.h"

#include "BLI_math.h"
#include "BLI_utildefines.h"

#include "../generic/py_capi_utils.h"

#ifndef MATH_STANDALONE
#  include "BLI_dynstr.h"
#endif

/**
 * Higher dimensions are supported, for many common operations
 * (dealing with vector/matrix multiply or handling as 3D locations)
 * stack memory is used with a fixed size - defined here.
 */
#define MAX_DIMENSIONS 4

/**
 * Swizzle axes get packed into a single value that is used as a closure. Each
 * axis uses SWIZZLE_BITS_PER_AXIS bits. The first bit (SWIZZLE_VALID_AXIS) is
 * used as a sentinel: if it is unset, the axis is not valid.
 */
#define SWIZZLE_BITS_PER_AXIS 3
#define SWIZZLE_VALID_AXIS 0x4
#define SWIZZLE_AXIS 0x3

static PyObject *Vector_copy(VectorObject *self);
static PyObject *Vector_deepcopy(VectorObject *self, PyObject *args);

/* -------------------------------------------------------------------- */
/** \name Utilities
 * \{ */

/**
 * Row vector multiplication - (Vector * Matrix)
 * <pre>
 * [x][y][z] * [1][4][7]
 *             [2][5][8]
 *             [3][6][9]
 * </pre>
 * \note vector/matrix multiplication is not commutative.
 */
static int row_vector_multiplication(float r_vec[MAX_DIMENSIONS],
                                     VectorObject *vec,
                                     MatrixObject *mat)
{
  float vec_cpy[MAX_DIMENSIONS];
  int row, col, z = 0, vec_num = vec->vec_num;

  if (mat->row_num != vec_num) {
    if (mat->row_num == 4 && vec_num == 3) {
      vec_cpy[3] = 1.0f;
    }
    else {
      PyErr_SetString(PyExc_ValueError,
                      "vector * matrix: matrix column size "
                      "and the vector size must be the same");
      return -1;
    }
  }

  if (BaseMath_ReadCallback(vec) == -1 || BaseMath_ReadCallback(mat) == -1) {
    return -1;
  }

  memcpy(vec_cpy, vec->vec, vec_num * sizeof(float));

  r_vec[3] = 1.0f;
  /* Multiplication. */
  for (col = 0; col < mat->col_num; col++) {
    double dot = 0.0;
    for (row = 0; row < mat->row_num; row++) {
      dot += (double)(MATRIX_ITEM(mat, row, col) * vec_cpy[row]);
    }
    r_vec[z++] = (float)dot;
  }
  return 0;
}

static PyObject *vec__apply_to_copy(PyObject *(*vec_func)(VectorObject *), VectorObject *self)
{
  PyObject *ret = Vector_copy(self);
  PyObject *ret_dummy = vec_func((VectorObject *)ret);
  if (ret_dummy) {
    Py_DECREF(ret_dummy);
    return (PyObject *)ret;
  }
  /* error */
  Py_DECREF(ret);
  return NULL;
}

/** \note #BaseMath_ReadCallback must be called beforehand. */
static PyObject *Vector_to_tuple_ex(VectorObject *self, int ndigits)
{
  PyObject *ret;
  int i;

  ret = PyTuple_New(self->vec_num);

  if (ndigits >= 0) {
    for (i = 0; i < self->vec_num; i++) {
      PyTuple_SET_ITEM(ret, i, PyFloat_FromDouble(double_round((double)self->vec[i], ndigits)));
    }
  }
  else {
    for (i = 0; i < self->vec_num; i++) {
      PyTuple_SET_ITEM(ret, i, PyFloat_FromDouble(self->vec[i]));
    }
  }

  return ret;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vector Type: `__new__` / `mathutils.Vector()`
 * \{ */

/**
 * Supports 2D, 3D, and 4D vector objects both int and float values
 * accepted. Mixed float and int values accepted. Ints are parsed to float
 */
static PyObject *Vector_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
  float *vec = NULL;
  int vec_num = 3; /* default to a 3D vector */

  if (kwds && PyDict_Size(kwds)) {
    PyErr_SetString(PyExc_TypeError,
                    "Vector(): "
                    "takes no keyword args");
    return NULL;
  }

  switch (PyTuple_GET_SIZE(args)) {
    case 0:
      vec = PyMem_Malloc(vec_num * sizeof(float));

      if (vec == NULL) {
        PyErr_SetString(PyExc_MemoryError,
                        "Vector(): "
                        "problem allocating pointer space");
        return NULL;
      }

      copy_vn_fl(vec, vec_num, 0.0f);
      break;
    case 1:
      if ((vec_num = mathutils_array_parse_alloc(
               &vec, 2, PyTuple_GET_ITEM(args, 0), "mathutils.Vector()")) == -1) {
        return NULL;
      }
      break;
    default:
      PyErr_SetString(PyExc_TypeError,
                      "mathutils.Vector(): "
                      "more than a single arg given");
      return NULL;
  }
  return Vector_CreatePyObject_alloc(vec, vec_num, type);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vector Class Methods
 * \{ */

PyDoc_STRVAR(C_Vector_Fill_doc,
             ".. classmethod:: Fill(size, fill=0.0)\n"
             "\n"
             "   Create a vector of length size with all values set to fill.\n"
             "\n"
             "   :arg size: The length of the vector to be created.\n"
             "   :type size: int\n"
             "   :arg fill: The value used to fill the vector.\n"
             "   :type fill: float\n");
static PyObject *C_Vector_Fill(PyObject *cls, PyObject *args)
{
  float *vec;
  int vec_num;
  float fill = 0.0f;

  if (!PyArg_ParseTuple(args, "i|f:Vector.Fill", &vec_num, &fill)) {
    return NULL;
  }

  if (vec_num < 2) {
    PyErr_SetString(PyExc_RuntimeError, "Vector(): invalid size");
    return NULL;
  }

  vec = PyMem_Malloc(vec_num * sizeof(float));

  if (vec == NULL) {
    PyErr_SetString(PyExc_MemoryError,
                    "Vector.Fill(): "
                    "problem allocating pointer space");
    return NULL;
  }

  copy_vn_fl(vec, vec_num, fill);

  return Vector_CreatePyObject_alloc(vec, vec_num, (PyTypeObject *)cls);
}

PyDoc_STRVAR(C_Vector_Range_doc,
             ".. classmethod:: Range(start, stop, step=1)\n"
             "\n"
             "   Create a filled with a range of values.\n"
             "\n"
             "   :arg start: The start of the range used to fill the vector.\n"
             "   :type start: int\n"
             "   :arg stop: The end of the range used to fill the vector.\n"
             "   :type stop: int\n"
             "   :arg step: The step between successive values in the vector.\n"
             "   :type step: int\n");
static PyObject *C_Vector_Range(PyObject *cls, PyObject *args)
{
  float *vec;
  int stop, vec_num;
  int start = 0;
  int step = 1;

  if (!PyArg_ParseTuple(args, "i|ii:Vector.Range", &start, &stop, &step)) {
    return NULL;
  }

  switch (PyTuple_GET_SIZE(args)) {
    case 1:
      vec_num = start;
      start = 0;
      break;
    case 2:
      if (start >= stop) {
        PyErr_SetString(PyExc_RuntimeError,
                        "Start value is larger "
                        "than the stop value");
        return NULL;
      }

      vec_num = stop - start;
      break;
    default:
      if (start >= stop) {
        PyErr_SetString(PyExc_RuntimeError,
                        "Start value is larger "
                        "than the stop value");
        return NULL;
      }

      vec_num = (stop - start);

      if ((vec_num % step) != 0) {
        vec_num += step;
      }

      vec_num /= step;

      break;
  }

  if (vec_num < 2) {
    PyErr_SetString(PyExc_RuntimeError, "Vector(): invalid size");
    return NULL;
  }

  vec = PyMem_Malloc(vec_num * sizeof(float));

  if (vec == NULL) {
    PyErr_SetString(PyExc_MemoryError,
                    "Vector.Range(): "
                    "problem allocating pointer space");
    return NULL;
  }

  range_vn_fl(vec, vec_num, (float)start, (float)step);

  return Vector_CreatePyObject_alloc(vec, vec_num, (PyTypeObject *)cls);
}

PyDoc_STRVAR(C_Vector_Linspace_doc,
             ".. classmethod:: Linspace(start, stop, size)\n"
             "\n"
             "   Create a vector of the specified size which is filled with linearly spaced "
             "values between start and stop values.\n"
             "\n"
             "   :arg start: The start of the range used to fill the vector.\n"
             "   :type start: int\n"
             "   :arg stop: The end of the range used to fill the vector.\n"
             "   :type stop: int\n"
             "   :arg size: The size of the vector to be created.\n"
             "   :type size: int\n");
static PyObject *C_Vector_Linspace(PyObject *cls, PyObject *args)
{
  float *vec;
  int vec_num;
  float start, end, step;

  if (!PyArg_ParseTuple(args, "ffi:Vector.Linspace", &start, &end, &vec_num)) {
    return NULL;
  }

  if (vec_num < 2) {
    PyErr_SetString(PyExc_RuntimeError, "Vector.Linspace(): invalid size");
    return NULL;
  }

  step = (end - start) / (float)(vec_num - 1);

  vec = PyMem_Malloc(vec_num * sizeof(float));

  if (vec == NULL) {
    PyErr_SetString(PyExc_MemoryError,
                    "Vector.Linspace(): "
                    "problem allocating pointer space");
    return NULL;
  }

  range_vn_fl(vec, vec_num, start, step);

  return Vector_CreatePyObject_alloc(vec, vec_num, (PyTypeObject *)cls);
}

PyDoc_STRVAR(
    C_Vector_Repeat_doc,
    ".. classmethod:: Repeat(vector, size)\n"
    "\n"
    "   Create a vector by repeating the values in vector until the required size is reached.\n"
    "\n"
    "   :arg tuple: The vector to draw values from.\n"
    "   :type tuple: :class:`mathutils.Vector`\n"
    "   :arg size: The size of the vector to be created.\n"
    "   :type size: int\n");
static PyObject *C_Vector_Repeat(PyObject *cls, PyObject *args)
{
  float *vec;
  float *iter_vec = NULL;
  int i, vec_num, value_num;
  PyObject *value;

  if (!PyArg_ParseTuple(args, "Oi:Vector.Repeat", &value, &vec_num)) {
    return NULL;
  }

  if (vec_num < 2) {
    PyErr_SetString(PyExc_RuntimeError, "Vector.Repeat(): invalid vec_num");
    return NULL;
  }

  if ((value_num = mathutils_array_parse_alloc(
           &iter_vec, 2, value, "Vector.Repeat(vector, vec_num), invalid 'vector' arg")) == -1) {
    return NULL;
  }

  if (iter_vec == NULL) {
    PyErr_SetString(PyExc_MemoryError,
                    "Vector.Repeat(): "
                    "problem allocating pointer space");
    return NULL;
  }

  vec = PyMem_Malloc(vec_num * sizeof(float));

  if (vec == NULL) {
    PyMem_Free(iter_vec);
    PyErr_SetString(PyExc_MemoryError,
                    "Vector.Repeat(): "
                    "problem allocating pointer space");
    return NULL;
  }

  i = 0;
  while (i < vec_num) {
    vec[i] = iter_vec[i % value_num];
    i++;
  }

  PyMem_Free(iter_vec);

  return Vector_CreatePyObject_alloc(vec, vec_num, (PyTypeObject *)cls);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vector Methods: Zero
 * \{ */

PyDoc_STRVAR(Vector_zero_doc,
             ".. method:: zero()\n"
             "\n"
             "   Set all values to zero.\n");
static PyObject *Vector_zero(VectorObject *self)
{
  if (BaseMath_Prepare_ForWrite(self) == -1) {
    return NULL;
  }

  copy_vn_fl(self->vec, self->vec_num, 0.0f);

  if (BaseMath_WriteCallback(self) == -1) {
    return NULL;
  }

  Py_RETURN_NONE;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vector Methods: Normalize
 * \{ */

PyDoc_STRVAR(Vector_normalize_doc,
             ".. method:: normalize()\n"
             "\n"
             "   Normalize the vector, making the length of the vector always 1.0.\n"
             "\n"
             "   .. warning:: Normalizing a vector where all values are zero has no effect.\n"
             "\n"
             "   .. note:: Normalize works for vectors of all sizes,\n"
             "      however 4D Vectors w axis is left untouched.\n");
static PyObject *Vector_normalize(VectorObject *self)
{
  const int vec_num = (self->vec_num == 4 ? 3 : self->vec_num);
  if (BaseMath_ReadCallback_ForWrite(self) == -1) {
    return NULL;
  }

  normalize_vn(self->vec, vec_num);

  (void)BaseMath_WriteCallback(self);
  Py_RETURN_NONE;
}
PyDoc_STRVAR(Vector_normalized_doc,
             ".. method:: normalized()\n"
             "\n"
             "   Return a new, normalized vector.\n"
             "\n"
             "   :return: a normalized copy of the vector\n"
             "   :rtype: :class:`Vector`\n");
static PyObject *Vector_normalized(VectorObject *self)
{
  return vec__apply_to_copy(Vector_normalize, self);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vector Methods: Resize
 * \{ */

PyDoc_STRVAR(Vector_resize_doc,
             ".. method:: resize(size=3)\n"
             "\n"
             "   Resize the vector to have size number of elements.\n");
static PyObject *Vector_resize(VectorObject *self, PyObject *value)
{
  int vec_num;

  if (self->flag & BASE_MATH_FLAG_IS_WRAP) {
    PyErr_SetString(PyExc_TypeError,
                    "Vector.resize(): "
                    "cannot resize wrapped data - only python vectors");
    return NULL;
  }
  if (self->cb_user) {
    PyErr_SetString(PyExc_TypeError,
                    "Vector.resize(): "
                    "cannot resize a vector that has an owner");
    return NULL;
  }

  if ((vec_num = PyC_Long_AsI32(value)) == -1) {
    PyErr_SetString(PyExc_TypeError,
                    "Vector.resize(size): "
                    "expected size argument to be an integer");
    return NULL;
  }

  if (vec_num < 2) {
    PyErr_SetString(PyExc_RuntimeError, "Vector.resize(): invalid size");
    return NULL;
  }

  self->vec = PyMem_Realloc(self->vec, (vec_num * sizeof(float)));
  if (self->vec == NULL) {
    PyErr_SetString(PyExc_MemoryError,
                    "Vector.resize(): "
                    "problem allocating pointer space");
    return NULL;
  }

  /* If the vector has increased in length, set all new elements to 0.0f */
  if (vec_num > self->vec_num) {
    copy_vn_fl(self->vec + self->vec_num, vec_num - self->vec_num, 0.0f);
  }

  self->vec_num = vec_num;
  Py_RETURN_NONE;
}

PyDoc_STRVAR(Vector_resized_doc,
             ".. method:: resized(size=3)\n"
             "\n"
             "   Return a resized copy of the vector with size number of elements.\n"
             "\n"
             "   :return: a new vector\n"
             "   :rtype: :class:`Vector`\n");
static PyObject *Vector_resized(VectorObject *self, PyObject *value)
{
  int vec_num;
  float *vec;

  if ((vec_num = PyLong_AsLong(value)) == -1) {
    return NULL;
  }

  if (vec_num < 2) {
    PyErr_SetString(PyExc_RuntimeError, "Vector.resized(): invalid size");
    return NULL;
  }

  vec = PyMem_Malloc(vec_num * sizeof(float));

  if (vec == NULL) {
    PyErr_SetString(PyExc_MemoryError,
                    "Vector.resized(): "
                    "problem allocating pointer space");
    return NULL;
  }

  copy_vn_fl(vec, vec_num, 0.0f);
  memcpy(vec, self->vec, self->vec_num * sizeof(float));

  return Vector_CreatePyObject_alloc(vec, vec_num, NULL);
}

PyDoc_STRVAR(Vector_resize_2d_doc,
             ".. method:: resize_2d()\n"
             "\n"
             "   Resize the vector to 2D  (x, y).\n");
static PyObject *Vector_resize_2d(VectorObject *self)
{
  if (self->flag & BASE_MATH_FLAG_IS_WRAP) {
    PyErr_SetString(PyExc_TypeError,
                    "Vector.resize_2d(): "
                    "cannot resize wrapped data - only python vectors");
    return NULL;
  }
  if (self->cb_user) {
    PyErr_SetString(PyExc_TypeError,
                    "Vector.resize_2d(): "
                    "cannot resize a vector that has an owner");
    return NULL;
  }

  self->vec = PyMem_Realloc(self->vec, sizeof(float[2]));
  if (self->vec == NULL) {
    PyErr_SetString(PyExc_MemoryError,
                    "Vector.resize_2d(): "
                    "problem allocating pointer space");
    return NULL;
  }

  self->vec_num = 2;
  Py_RETURN_NONE;
}

PyDoc_STRVAR(Vector_resize_3d_doc,
             ".. method:: resize_3d()\n"
             "\n"
             "   Resize the vector to 3D  (x, y, z).\n");
static PyObject *Vector_resize_3d(VectorObject *self)
{
  if (self->flag & BASE_MATH_FLAG_IS_WRAP) {
    PyErr_SetString(PyExc_TypeError,
                    "Vector.resize_3d(): "
                    "cannot resize wrapped data - only python vectors");
    return NULL;
  }
  if (self->cb_user) {
    PyErr_SetString(PyExc_TypeError,
                    "Vector.resize_3d(): "
                    "cannot resize a vector that has an owner");
    return NULL;
  }

  self->vec = PyMem_Realloc(self->vec, sizeof(float[3]));
  if (self->vec == NULL) {
    PyErr_SetString(PyExc_MemoryError,
                    "Vector.resize_3d(): "
                    "problem allocating pointer space");
    return NULL;
  }

  if (self->vec_num == 2) {
    self->vec[2] = 0.0f;
  }

  self->vec_num = 3;
  Py_RETURN_NONE;
}

PyDoc_STRVAR(Vector_resize_4d_doc,
             ".. method:: resize_4d()\n"
             "\n"
             "   Resize the vector to 4D (x, y, z, w).\n");
static PyObject *Vector_resize_4d(VectorObject *self)
{
  if (self->flag & BASE_MATH_FLAG_IS_WRAP) {
    PyErr_SetString(PyExc_TypeError,
                    "Vector.resize_4d(): "
                    "cannot resize wrapped data - only python vectors");
    return NULL;
  }
  if (self->cb_user) {
    PyErr_SetString(PyExc_TypeError,
                    "Vector.resize_4d(): "
                    "cannot resize a vector that has an owner");
    return NULL;
  }

  self->vec = PyMem_Realloc(self->vec, sizeof(float[4]));
  if (self->vec == NULL) {
    PyErr_SetString(PyExc_MemoryError,
                    "Vector.resize_4d(): "
                    "problem allocating pointer space");
    return NULL;
  }

  if (self->vec_num == 2) {
    self->vec[2] = 0.0f;
    self->vec[3] = 1.0f;
  }
  else if (self->vec_num == 3) {
    self->vec[3] = 1.0f;
  }
  self->vec_num = 4;
  Py_RETURN_NONE;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vector Methods: To N-dimensions
 * \{ */

PyDoc_STRVAR(Vector_to_2d_doc,
             ".. method:: to_2d()\n"
             "\n"
             "   Return a 2d copy of the vector.\n"
             "\n"
             "   :return: a new vector\n"
             "   :rtype: :class:`Vector`\n");
static PyObject *Vector_to_2d(VectorObject *self)
{
  if (BaseMath_ReadCallback(self) == -1) {
    return NULL;
  }

  return Vector_CreatePyObject(self->vec, 2, Py_TYPE(self));
}
PyDoc_STRVAR(Vector_to_3d_doc,
             ".. method:: to_3d()\n"
             "\n"
             "   Return a 3d copy of the vector.\n"
             "\n"
             "   :return: a new vector\n"
             "   :rtype: :class:`Vector`\n");
static PyObject *Vector_to_3d(VectorObject *self)
{
  float tvec[3] = {0.0f};

  if (BaseMath_ReadCallback(self) == -1) {
    return NULL;
  }

  memcpy(tvec, self->vec, sizeof(float) * MIN2(self->vec_num, 3));
  return Vector_CreatePyObject(tvec, 3, Py_TYPE(self));
}
PyDoc_STRVAR(Vector_to_4d_doc,
             ".. method:: to_4d()\n"
             "\n"
             "   Return a 4d copy of the vector.\n"
             "\n"
             "   :return: a new vector\n"
             "   :rtype: :class:`Vector`\n");
static PyObject *Vector_to_4d(VectorObject *self)
{
  float tvec[4] = {0.0f, 0.0f, 0.0f, 1.0f};

  if (BaseMath_ReadCallback(self) == -1) {
    return NULL;
  }

  memcpy(tvec, self->vec, sizeof(float) * MIN2(self->vec_num, 4));
  return Vector_CreatePyObject(tvec, 4, Py_TYPE(self));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vector Methods: To Tuple
 * \{ */

PyDoc_STRVAR(Vector_to_tuple_doc,
             ".. method:: to_tuple(precision=-1)\n"
             "\n"
             "   Return this vector as a tuple with.\n"
             "\n"
             "   :arg precision: The number to round the value to in [-1, 21].\n"
             "   :type precision: int\n"
             "   :return: the values of the vector rounded by *precision*\n"
             "   :rtype: tuple\n");
static PyObject *Vector_to_tuple(VectorObject *self, PyObject *args)
{
  int ndigits = 0;

  if (!PyArg_ParseTuple(args, "|i:to_tuple", &ndigits)) {
    return NULL;
  }

  if (ndigits > 22 || ndigits < 0) {
    PyErr_SetString(PyExc_ValueError,
                    "Vector.to_tuple(ndigits): "
                    "ndigits must be between 0 and 21");
    return NULL;
  }

  if (PyTuple_GET_SIZE(args) == 0) {
    ndigits = -1;
  }

  if (BaseMath_ReadCallback(self) == -1) {
    return NULL;
  }

  return Vector_to_tuple_ex(self, ndigits);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vector Methods: To Track Quaternion
 * \{ */

PyDoc_STRVAR(Vector_to_track_quat_doc,
             ".. method:: to_track_quat(track, up)\n"
             "\n"
             "   Return a quaternion rotation from the vector and the track and up axis.\n"
             "\n"
             "   :arg track: Track axis in ['X', 'Y', 'Z', '-X', '-Y', '-Z'].\n"
             "   :type track: string\n"
             "   :arg up: Up axis in ['X', 'Y', 'Z'].\n"
             "   :type up: string\n"
             "   :return: rotation from the vector and the track and up axis.\n"
             "   :rtype: :class:`Quaternion`\n");
static PyObject *Vector_to_track_quat(VectorObject *self, PyObject *args)
{
  float vec[3], quat[4];
  const char *strack = NULL;
  const char *sup = NULL;
  short track = 2, up = 1;

  if (!PyArg_ParseTuple(args, "|ss:to_track_quat", &strack, &sup)) {
    return NULL;
  }

  if (self->vec_num != 3) {
    PyErr_SetString(PyExc_TypeError,
                    "Vector.to_track_quat(): "
                    "only for 3D vectors");
    return NULL;
  }

  if (BaseMath_ReadCallback(self) == -1) {
    return NULL;
  }

  if (strack) {
    const char *axis_err_msg = "only X, -X, Y, -Y, Z or -Z for track axis";

    if (strlen(strack) == 2) {
      if (strack[0] == '-') {
        switch (strack[1]) {
          case 'X':
            track = 3;
            break;
          case 'Y':
            track = 4;
            break;
          case 'Z':
            track = 5;
            break;
          default:
            PyErr_SetString(PyExc_ValueError, axis_err_msg);
            return NULL;
        }
      }
      else {
        PyErr_SetString(PyExc_ValueError, axis_err_msg);
        return NULL;
      }
    }
    else if (strlen(strack) == 1) {
      switch (strack[0]) {
        case '-':
        case 'X':
          track = 0;
          break;
        case 'Y':
          track = 1;
          break;
        case 'Z':
          track = 2;
          break;
        default:
          PyErr_SetString(PyExc_ValueError, axis_err_msg);
          return NULL;
      }
    }
    else {
      PyErr_SetString(PyExc_ValueError, axis_err_msg);
      return NULL;
    }
  }

  if (sup) {
    const char *axis_err_msg = "only X, Y or Z for up axis";
    if (strlen(sup) == 1) {
      switch (*sup) {
        case 'X':
          up = 0;
          break;
        case 'Y':
          up = 1;
          break;
        case 'Z':
          up = 2;
          break;
        default:
          PyErr_SetString(PyExc_ValueError, axis_err_msg);
          return NULL;
      }
    }
    else {
      PyErr_SetString(PyExc_ValueError, axis_err_msg);
      return NULL;
    }
  }

  if (track == up) {
    PyErr_SetString(PyExc_ValueError, "Can't have the same axis for track and up");
    return NULL;
  }

  /* Flip vector around, since #vec_to_quat expect a vector from target to tracking object
   * and the python function expects the inverse (a vector to the target). */
  negate_v3_v3(vec, self->vec);

  vec_to_quat(quat, vec, track, up);

  return Quaternion_CreatePyObject(quat, NULL);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vector Methods: Orthogonal
 * \{ */

PyDoc_STRVAR(
    Vector_orthogonal_doc,
    ".. method:: orthogonal()\n"
    "\n"
    "   Return a perpendicular vector.\n"
    "\n"
    "   :return: a new vector 90 degrees from this vector.\n"
    "   :rtype: :class:`Vector`\n"
    "\n"
    "   .. note:: the axis is undefined, only use when any orthogonal vector is acceptable.\n");
static PyObject *Vector_orthogonal(VectorObject *self)
{
  float vec[3];

  if (self->vec_num > 3) {
    PyErr_SetString(PyExc_TypeError,
                    "Vector.orthogonal(): "
                    "Vector must be 3D or 2D");
    return NULL;
  }

  if (BaseMath_ReadCallback(self) == -1) {
    return NULL;
  }

  if (self->vec_num == 3) {
    ortho_v3_v3(vec, self->vec);
  }
  else {
    ortho_v2_v2(vec, self->vec);
  }

  return Vector_CreatePyObject(vec, self->vec_num, Py_TYPE(self));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vector Methods: Reflect
 *
 * `Vector.reflect(mirror)`: return a reflected vector on the mirror normal:
 * `vec - ((2 * dot(vec, mirror)) * mirror)`.
 * \{ */

PyDoc_STRVAR(Vector_reflect_doc,
             ".. method:: reflect(mirror)\n"
             "\n"
             "   Return the reflection vector from the *mirror* argument.\n"
             "\n"
             "   :arg mirror: This vector could be a normal from the reflecting surface.\n"
             "   :type mirror: :class:`Vector`\n"
             "   :return: The reflected vector matching the size of this vector.\n"
             "   :rtype: :class:`Vector`\n");
static PyObject *Vector_reflect(VectorObject *self, PyObject *value)
{
  int value_num;
  float mirror[3], vec[3];
  float reflect[3] = {0.0f};
  float tvec[MAX_DIMENSIONS];

  if (BaseMath_ReadCallback(self) == -1) {
    return NULL;
  }

  if ((value_num = mathutils_array_parse(
           tvec, 2, 4, value, "Vector.reflect(other), invalid 'other' arg")) == -1) {
    return NULL;
  }

  if (self->vec_num < 2 || self->vec_num > 4) {
    PyErr_SetString(PyExc_ValueError, "Vector must be 2D, 3D or 4D");
    return NULL;
  }

  mirror[0] = tvec[0];
  mirror[1] = tvec[1];
  mirror[2] = (value_num > 2) ? tvec[2] : 0.0f;

  vec[0] = self->vec[0];
  vec[1] = self->vec[1];
  vec[2] = (value_num > 2) ? self->vec[2] : 0.0f;

  normalize_v3(mirror);
  reflect_v3_v3v3(reflect, vec, mirror);

  return Vector_CreatePyObject(reflect, self->vec_num, Py_TYPE(self));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vector Methods: Cross Product
 * \{ */

PyDoc_STRVAR(Vector_cross_doc,
             ".. method:: cross(other)\n"
             "\n"
             "   Return the cross product of this vector and another.\n"
             "\n"
             "   :arg other: The other vector to perform the cross product with.\n"
             "   :type other: :class:`Vector`\n"
             "   :return: The cross product.\n"
             "   :rtype: :class:`Vector` or float when 2D vectors are used\n"
             "\n"
             "   .. note:: both vectors must be 2D or 3D\n");
static PyObject *Vector_cross(VectorObject *self, PyObject *value)
{
  PyObject *ret;
  float tvec[3];

  if (BaseMath_ReadCallback(self) == -1) {
    return NULL;
  }

  if (self->vec_num > 3) {
    PyErr_SetString(PyExc_ValueError, "Vector must be 2D or 3D");
    return NULL;
  }

  if (mathutils_array_parse(
          tvec, self->vec_num, self->vec_num, value, "Vector.cross(other), invalid 'other' arg") ==
      -1) {
    return NULL;
  }

  if (self->vec_num == 3) {
    ret = Vector_CreatePyObject(NULL, 3, Py_TYPE(self));
    cross_v3_v3v3(((VectorObject *)ret)->vec, self->vec, tvec);
  }
  else {
    /* size == 2 */
    ret = PyFloat_FromDouble(cross_v2v2(self->vec, tvec));
  }
  return ret;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vector Methods: Dot Product
 * \{ */

PyDoc_STRVAR(Vector_dot_doc,
             ".. method:: dot(other)\n"
             "\n"
             "   Return the dot product of this vector and another.\n"
             "\n"
             "   :arg other: The other vector to perform the dot product with.\n"
             "   :type other: :class:`Vector`\n"
             "   :return: The dot product.\n"
             "   :rtype: float\n");
static PyObject *Vector_dot(VectorObject *self, PyObject *value)
{
  float *tvec;
  PyObject *ret;

  if (BaseMath_ReadCallback(self) == -1) {
    return NULL;
  }

  if (mathutils_array_parse_alloc(
          &tvec, self->vec_num, value, "Vector.dot(other), invalid 'other' arg") == -1) {
    return NULL;
  }

  ret = PyFloat_FromDouble(dot_vn_vn(self->vec, tvec, self->vec_num));
  PyMem_Free(tvec);
  return ret;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vector Methods: Angle
 * \{ */

PyDoc_STRVAR(
    Vector_angle_doc,
    ".. function:: angle(other, fallback=None)\n"
    "\n"
    "   Return the angle between two vectors.\n"
    "\n"
    "   :arg other: another vector to compare the angle with\n"
    "   :type other: :class:`Vector`\n"
    "   :arg fallback: return this when the angle can't be calculated (zero length vector),\n"
    "      (instead of raising a :exc:`ValueError`).\n"
    "   :type fallback: any\n"
    "   :return: angle in radians or fallback when given\n"
    "   :rtype: float\n");
static PyObject *Vector_angle(VectorObject *self, PyObject *args)
{
  const int vec_num = MIN2(self->vec_num, 3); /* 4D angle makes no sense */
  float tvec[MAX_DIMENSIONS];
  PyObject *value;
  double dot = 0.0f, dot_self = 0.0f, dot_other = 0.0f;
  int x;
  PyObject *fallback = NULL;

  if (!PyArg_ParseTuple(args, "O|O:angle", &value, &fallback)) {
    return NULL;
  }

  if (BaseMath_ReadCallback(self) == -1) {
    return NULL;
  }

  /* don't use clamped size, rule of thumb is vector sizes must match,
   * even though n this case 'w' is ignored */
  if (mathutils_array_parse(
          tvec, self->vec_num, self->vec_num, value, "Vector.angle(other), invalid 'other' arg") ==
      -1) {
    return NULL;
  }

  if (self->vec_num > 4) {
    PyErr_SetString(PyExc_ValueError, "Vector must be 2D, 3D or 4D");
    return NULL;
  }

  for (x = 0; x < vec_num; x++) {
    dot_self += (double)self->vec[x] * (double)self->vec[x];
    dot_other += (double)tvec[x] * (double)tvec[x];
    dot += (double)self->vec[x] * (double)tvec[x];
  }

  if (!dot_self || !dot_other) {
    /* avoid exception */
    if (fallback) {
      Py_INCREF(fallback);
      return fallback;
    }

    PyErr_SetString(PyExc_ValueError,
                    "Vector.angle(other): "
                    "zero length vectors have no valid angle");
    return NULL;
  }

  return PyFloat_FromDouble(saacos(dot / (sqrt(dot_self) * sqrt(dot_other))));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vector Methods: Angle Signed
 * \{ */

PyDoc_STRVAR(
    Vector_angle_signed_doc,
    ".. function:: angle_signed(other, fallback)\n"
    "\n"
    "   Return the signed angle between two 2D vectors (clockwise is positive).\n"
    "\n"
    "   :arg other: another vector to compare the angle with\n"
    "   :type other: :class:`Vector`\n"
    "   :arg fallback: return this when the angle can't be calculated (zero length vector),\n"
    "      (instead of raising a :exc:`ValueError`).\n"
    "   :type fallback: any\n"
    "   :return: angle in radians or fallback when given\n"
    "   :rtype: float\n");
static PyObject *Vector_angle_signed(VectorObject *self, PyObject *args)
{
  float tvec[2];

  PyObject *value;
  PyObject *fallback = NULL;

  if (!PyArg_ParseTuple(args, "O|O:angle_signed", &value, &fallback)) {
    return NULL;
  }

  if (BaseMath_ReadCallback(self) == -1) {
    return NULL;
  }

  if (mathutils_array_parse(
          tvec, 2, 2, value, "Vector.angle_signed(other), invalid 'other' arg") == -1) {
    return NULL;
  }

  if (self->vec_num != 2) {
    PyErr_SetString(PyExc_ValueError, "Vector must be 2D");
    return NULL;
  }

  if (is_zero_v2(self->vec) || is_zero_v2(tvec)) {
    /* avoid exception */
    if (fallback) {
      Py_INCREF(fallback);
      return fallback;
    }

    PyErr_SetString(PyExc_ValueError,
                    "Vector.angle_signed(other): "
                    "zero length vectors have no valid angle");
    return NULL;
  }

  return PyFloat_FromDouble(angle_signed_v2v2(self->vec, tvec));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vector Methods: Rotation Difference
 * \{ */

PyDoc_STRVAR(Vector_rotation_difference_doc,
             ".. function:: rotation_difference(other)\n"
             "\n"
             "   Returns a quaternion representing the rotational difference between this\n"
             "   vector and another.\n"
             "\n"
             "   :arg other: second vector.\n"
             "   :type other: :class:`Vector`\n"
             "   :return: the rotational difference between the two vectors.\n"
             "   :rtype: :class:`Quaternion`\n"
             "\n"
             "   .. note:: 2D vectors raise an :exc:`AttributeError`.\n");
static PyObject *Vector_rotation_difference(VectorObject *self, PyObject *value)
{
  float quat[4], vec_a[3], vec_b[3];

  if (self->vec_num < 3 || self->vec_num > 4) {
    PyErr_SetString(PyExc_ValueError,
                    "vec.difference(value): "
                    "expects both vectors to be size 3 or 4");
    return NULL;
  }

  if (BaseMath_ReadCallback(self) == -1) {
    return NULL;
  }

  if (mathutils_array_parse(
          vec_b, 3, MAX_DIMENSIONS, value, "Vector.difference(other), invalid 'other' arg") ==
      -1) {
    return NULL;
  }

  normalize_v3_v3(vec_a, self->vec);
  normalize_v3(vec_b);

  rotation_between_vecs_to_quat(quat, vec_a, vec_b);

  return Quaternion_CreatePyObject(quat, NULL);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vector Methods: Project
 * \{ */

PyDoc_STRVAR(Vector_project_doc,
             ".. function:: project(other)\n"
             "\n"
             "   Return the projection of this vector onto the *other*.\n"
             "\n"
             "   :arg other: second vector.\n"
             "   :type other: :class:`Vector`\n"
             "   :return: the parallel projection vector\n"
             "   :rtype: :class:`Vector`\n");
static PyObject *Vector_project(VectorObject *self, PyObject *value)
{
  const int vec_num = self->vec_num;
  float *tvec;
  double dot = 0.0f, dot2 = 0.0f;
  int x;

  if (BaseMath_ReadCallback(self) == -1) {
    return NULL;
  }

  if (mathutils_array_parse_alloc(
          &tvec, vec_num, value, "Vector.project(other), invalid 'other' arg") == -1) {
    return NULL;
  }

  /* get dot products */
  for (x = 0; x < vec_num; x++) {
    dot += (double)(self->vec[x] * tvec[x]);
    dot2 += (double)(tvec[x] * tvec[x]);
  }
  /* projection */
  dot /= dot2;
  for (x = 0; x < vec_num; x++) {
    tvec[x] *= (float)dot;
  }
  return Vector_CreatePyObject_alloc(tvec, vec_num, Py_TYPE(self));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vector Methods: Linear Interpolation
 * \{ */

PyDoc_STRVAR(Vector_lerp_doc,
             ".. function:: lerp(other, factor)\n"
             "\n"
             "   Returns the interpolation of two vectors.\n"
             "\n"
             "   :arg other: value to interpolate with.\n"
             "   :type other: :class:`Vector`\n"
             "   :arg factor: The interpolation value in [0.0, 1.0].\n"
             "   :type factor: float\n"
             "   :return: The interpolated vector.\n"
             "   :rtype: :class:`Vector`\n");
static PyObject *Vector_lerp(VectorObject *self, PyObject *args)
{
  const int vec_num = self->vec_num;
  PyObject *value = NULL;
  float fac;
  float *tvec;

  if (!PyArg_ParseTuple(args, "Of:lerp", &value, &fac)) {
    return NULL;
  }

  if (BaseMath_ReadCallback(self) == -1) {
    return NULL;
  }

  if (mathutils_array_parse_alloc(
          &tvec, vec_num, value, "Vector.lerp(other), invalid 'other' arg") == -1) {
    return NULL;
  }

  interp_vn_vn(tvec, self->vec, 1.0f - fac, vec_num);

  return Vector_CreatePyObject_alloc(tvec, vec_num, Py_TYPE(self));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vector Methods: Spherical Interpolation
 * \{ */

PyDoc_STRVAR(Vector_slerp_doc,
             ".. function:: slerp(other, factor, fallback=None)\n"
             "\n"
             "   Returns the interpolation of two non-zero vectors (spherical coordinates).\n"
             "\n"
             "   :arg other: value to interpolate with.\n"
             "   :type other: :class:`Vector`\n"
             "   :arg factor: The interpolation value typically in [0.0, 1.0].\n"
             "   :type factor: float\n"
             "   :arg fallback: return this when the vector can't be calculated (zero length "
             "vector or direct opposites),\n"
             "      (instead of raising a :exc:`ValueError`).\n"
             "   :type fallback: any\n"
             "   :return: The interpolated vector.\n"
             "   :rtype: :class:`Vector`\n");
static PyObject *Vector_slerp(VectorObject *self, PyObject *args)
{
  const int vec_num = self->vec_num;
  PyObject *value = NULL;
  float fac, cosom, w[2];
  float self_vec[3], other_vec[3], ret_vec[3];
  float self_len_sq, other_len_sq;
  int x;
  PyObject *fallback = NULL;

  if (!PyArg_ParseTuple(args, "Of|O:slerp", &value, &fac, &fallback)) {
    return NULL;
  }

  if (BaseMath_ReadCallback(self) == -1) {
    return NULL;
  }

  if (self->vec_num > 3) {
    PyErr_SetString(PyExc_ValueError, "Vector must be 2D or 3D");
    return NULL;
  }

  if (mathutils_array_parse(
          other_vec, vec_num, vec_num, value, "Vector.slerp(other), invalid 'other' arg") == -1) {
    return NULL;
  }

  self_len_sq = normalize_vn_vn(self_vec, self->vec, vec_num);
  other_len_sq = normalize_vn(other_vec, vec_num);

  /* use fallbacks for zero length vectors */
  if (UNLIKELY((self_len_sq < FLT_EPSILON) || (other_len_sq < FLT_EPSILON))) {
    /* avoid exception */
    if (fallback) {
      Py_INCREF(fallback);
      return fallback;
    }

    PyErr_SetString(PyExc_ValueError,
                    "Vector.slerp(): "
                    "zero length vectors unsupported");
    return NULL;
  }

  /* We have sane state, execute slerp */
  cosom = (float)dot_vn_vn(self_vec, other_vec, vec_num);

  /* direct opposite, can't slerp */
  if (UNLIKELY(cosom < (-1.0f + FLT_EPSILON))) {
    /* avoid exception */
    if (fallback) {
      Py_INCREF(fallback);
      return fallback;
    }

    PyErr_SetString(PyExc_ValueError,
                    "Vector.slerp(): "
                    "opposite vectors unsupported");
    return NULL;
  }

  interp_dot_slerp(fac, cosom, w);

  for (x = 0; x < vec_num; x++) {
    ret_vec[x] = (w[0] * self_vec[x]) + (w[1] * other_vec[x]);
  }

  return Vector_CreatePyObject(ret_vec, vec_num, Py_TYPE(self));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vector Methods: Rotate
 * \{ */

PyDoc_STRVAR(
    Vector_rotate_doc,
    ".. function:: rotate(other)\n"
    "\n"
    "   Rotate the vector by a rotation value.\n"
    "\n"
    "   .. note:: 2D vectors are a special case that can only be rotated by a 2x2 matrix.\n"
    "\n"
    "   :arg other: rotation component of mathutils value\n"
    "   :type other: :class:`Euler`, :class:`Quaternion` or :class:`Matrix`\n");
static PyObject *Vector_rotate(VectorObject *self, PyObject *value)
{
  if (BaseMath_ReadCallback_ForWrite(self) == -1) {
    return NULL;
  }

  if (self->vec_num == 2) {
    /* Special case for 2D Vector with 2x2 matrix, so we avoid resizing it to a 3x3. */
    float other_rmat[2][2];
    MatrixObject *pymat;
    if (!Matrix_Parse2x2(value, &pymat)) {
      return NULL;
    }
    normalize_m2_m2(other_rmat, (const float(*)[2])pymat->matrix);
    /* Equivalent to a rotation along the Z axis. */
    mul_m2_v2(other_rmat, self->vec);
  }
  else {
    float other_rmat[3][3];

    if (mathutils_any_to_rotmat(other_rmat, value, "Vector.rotate(value)") == -1) {
      return NULL;
    }

    mul_m3_v3(other_rmat, self->vec);
  }

  (void)BaseMath_WriteCallback(self);
  Py_RETURN_NONE;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vector Methods: Negate
 * \{ */

PyDoc_STRVAR(Vector_negate_doc,
             ".. method:: negate()\n"
             "\n"
             "   Set all values to their negative.\n");
static PyObject *Vector_negate(VectorObject *self)
{
  if (BaseMath_ReadCallback(self) == -1) {
    return NULL;
  }

  negate_vn(self->vec, self->vec_num);

  (void)BaseMath_WriteCallback(self); /* already checked for error */
  Py_RETURN_NONE;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vector Methods: Copy/Deep-Copy
 * \{ */

PyDoc_STRVAR(Vector_copy_doc,
             ".. function:: copy()\n"
             "\n"
             "   Returns a copy of this vector.\n"
             "\n"
             "   :return: A copy of the vector.\n"
             "   :rtype: :class:`Vector`\n"
             "\n"
             "   .. note:: use this to get a copy of a wrapped vector with\n"
             "      no reference to the original data.\n");
static PyObject *Vector_copy(VectorObject *self)
{
  if (BaseMath_ReadCallback(self) == -1) {
    return NULL;
  }

  return Vector_CreatePyObject(self->vec, self->vec_num, Py_TYPE(self));
}
static PyObject *Vector_deepcopy(VectorObject *self, PyObject *args)
{
  if (!PyC_CheckArgs_DeepCopy(args)) {
    return NULL;
  }
  return Vector_copy(self);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vector Type: `__repr__` & `__str__`
 * \{ */

static PyObject *Vector_repr(VectorObject *self)
{
  PyObject *ret, *tuple;

  if (BaseMath_ReadCallback(self) == -1) {
    return NULL;
  }

  tuple = Vector_to_tuple_ex(self, -1);
  ret = PyUnicode_FromFormat("Vector(%R)", tuple);
  Py_DECREF(tuple);
  return ret;
}

#ifndef MATH_STANDALONE
static PyObject *Vector_str(VectorObject *self)
{
  int i;

  DynStr *ds;

  if (BaseMath_ReadCallback(self) == -1) {
    return NULL;
  }

  ds = BLI_dynstr_new();

  BLI_dynstr_append(ds, "<Vector (");

  for (i = 0; i < self->vec_num; i++) {
    BLI_dynstr_appendf(ds, i ? ", %.4f" : "%.4f", self->vec[i]);
  }

  BLI_dynstr_append(ds, ")>");

  return mathutils_dynstr_to_py(ds); /* frees ds */
}
#endif

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vector Type: Rich Compare
 * \{ */

static PyObject *Vector_richcmpr(PyObject *objectA, PyObject *objectB, int comparison_type)
{
  VectorObject *vecA = NULL, *vecB = NULL;
  int result = 0;
  const double epsilon = 0.000001f;
  double lenA, lenB;

  if (!VectorObject_Check(objectA) || !VectorObject_Check(objectB)) {
    if (comparison_type == Py_NE) {
      Py_RETURN_TRUE;
    }

    Py_RETURN_FALSE;
  }
  vecA = (VectorObject *)objectA;
  vecB = (VectorObject *)objectB;

  if (BaseMath_ReadCallback(vecA) == -1 || BaseMath_ReadCallback(vecB) == -1) {
    return NULL;
  }

  if (vecA->vec_num != vecB->vec_num) {
    if (comparison_type == Py_NE) {
      Py_RETURN_TRUE;
    }

    Py_RETURN_FALSE;
  }

  switch (comparison_type) {
    case Py_LT:
      lenA = len_squared_vn(vecA->vec, vecA->vec_num);
      lenB = len_squared_vn(vecB->vec, vecB->vec_num);
      if (lenA < lenB) {
        result = 1;
      }
      break;
    case Py_LE:
      lenA = len_squared_vn(vecA->vec, vecA->vec_num);
      lenB = len_squared_vn(vecB->vec, vecB->vec_num);
      if (lenA < lenB) {
        result = 1;
      }
      else {
        result = (((lenA + epsilon) > lenB) && ((lenA - epsilon) < lenB));
      }
      break;
    case Py_EQ:
      result = EXPP_VectorsAreEqual(vecA->vec, vecB->vec, vecA->vec_num, 1);
      break;
    case Py_NE:
      result = !EXPP_VectorsAreEqual(vecA->vec, vecB->vec, vecA->vec_num, 1);
      break;
    case Py_GT:
      lenA = len_squared_vn(vecA->vec, vecA->vec_num);
      lenB = len_squared_vn(vecB->vec, vecB->vec_num);
      if (lenA > lenB) {
        result = 1;
      }
      break;
    case Py_GE:
      lenA = len_squared_vn(vecA->vec, vecA->vec_num);
      lenB = len_squared_vn(vecB->vec, vecB->vec_num);
      if (lenA > lenB) {
        result = 1;
      }
      else {
        result = (((lenA + epsilon) > lenB) && ((lenA - epsilon) < lenB));
      }
      break;
    default:
      printf("The result of the comparison could not be evaluated");
      break;
  }
  if (result == 1) {
    Py_RETURN_TRUE;
  }

  Py_RETURN_FALSE;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vector Type: Hash (`__hash__`)
 * \{ */

static Py_hash_t Vector_hash(VectorObject *self)
{
  if (BaseMath_ReadCallback(self) == -1) {
    return -1;
  }

  if (BaseMathObject_Prepare_ForHash(self) == -1) {
    return -1;
  }

  return mathutils_array_hash(self->vec, self->vec_num);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vector Type: Sequence & Mapping Protocols Implementation
 * \{ */

/** Sequence length: `len(object)`. */
static int Vector_len(VectorObject *self)
{
  return self->vec_num;
}

static PyObject *vector_item_internal(VectorObject *self, int i, const bool is_attr)
{
  if (i < 0) {
    i = self->vec_num - i;
  }

  if (i < 0 || i >= self->vec_num) {
    if (is_attr) {
      PyErr_Format(PyExc_AttributeError,
                   "Vector.%c: unavailable on %dd vector",
                   *(((const char *)"xyzw") + i),
                   self->vec_num);
    }
    else {
      PyErr_SetString(PyExc_IndexError, "vector[index]: out of range");
    }
    return NULL;
  }

  if (BaseMath_ReadIndexCallback(self, i) == -1) {
    return NULL;
  }

  return PyFloat_FromDouble(self->vec[i]);
}

/** Sequence accessor (get): `x = object[i]`. */
static PyObject *Vector_item(VectorObject *self, int i)
{
  return vector_item_internal(self, i, false);
}

static int vector_ass_item_internal(VectorObject *self, int i, PyObject *value, const bool is_attr)
{
  float scalar;

  if (BaseMath_Prepare_ForWrite(self) == -1) {
    return -1;
  }

  if ((scalar = PyFloat_AsDouble(value)) == -1.0f && PyErr_Occurred()) {
    /* parsed item not a number */
    PyErr_SetString(PyExc_TypeError,
                    "vector[index] = x: "
                    "assigned value not a number");
    return -1;
  }

  if (i < 0) {
    i = self->vec_num - i;
  }

  if (i < 0 || i >= self->vec_num) {
    if (is_attr) {
      PyErr_Format(PyExc_AttributeError,
                   "Vector.%c = x: unavailable on %dd vector",
                   *(((const char *)"xyzw") + i),
                   self->vec_num);
    }
    else {
      PyErr_SetString(PyExc_IndexError,
                      "vector[index] = x: "
                      "assignment index out of range");
    }
    return -1;
  }
  self->vec[i] = scalar;

  if (BaseMath_WriteIndexCallback(self, i) == -1) {
    return -1;
  }
  return 0;
}

/** Sequence accessor (set): `object[i] = x`. */
static int Vector_ass_item(VectorObject *self, int i, PyObject *value)
{
  return vector_ass_item_internal(self, i, value, false);
}

/** Sequence slice accessor (get): `x = object[i:j]`. */
static PyObject *Vector_slice(VectorObject *self, int begin, int end)
{
  PyObject *tuple;
  int count;

  if (BaseMath_ReadCallback(self) == -1) {
    return NULL;
  }

  CLAMP(begin, 0, self->vec_num);
  if (end < 0) {
    end = self->vec_num + end + 1;
  }
  CLAMP(end, 0, self->vec_num);
  begin = MIN2(begin, end);

  tuple = PyTuple_New(end - begin);
  for (count = begin; count < end; count++) {
    PyTuple_SET_ITEM(tuple, count - begin, PyFloat_FromDouble(self->vec[count]));
  }

  return tuple;
}

/** Sequence slice accessor (set): `object[i:j] = x`. */
static int Vector_ass_slice(VectorObject *self, int begin, int end, PyObject *seq)
{
  int vec_num = 0;
  float *vec = NULL;

  if (BaseMath_ReadCallback_ForWrite(self) == -1) {
    return -1;
  }

  CLAMP(begin, 0, self->vec_num);
  CLAMP(end, 0, self->vec_num);
  begin = MIN2(begin, end);

  vec_num = (end - begin);
  if (mathutils_array_parse_alloc(&vec, vec_num, seq, "vector[begin:end] = [...]") == -1) {
    return -1;
  }

  if (vec == NULL) {
    PyErr_SetString(PyExc_MemoryError,
                    "vec[:] = seq: "
                    "problem allocating pointer space");
    return -1;
  }

  /* Parsed well - now set in vector. */
  memcpy(self->vec + begin, vec, vec_num * sizeof(float));

  PyMem_Free(vec);

  if (BaseMath_WriteCallback(self) == -1) {
    return -1;
  }

  return 0;
}

/** Sequence generic subscript (get): `x = object[...]`. */
static PyObject *Vector_subscript(VectorObject *self, PyObject *item)
{
  if (PyIndex_Check(item)) {
    Py_ssize_t i;
    i = PyNumber_AsSsize_t(item, PyExc_IndexError);
    if (i == -1 && PyErr_Occurred()) {
      return NULL;
    }
    if (i < 0) {
      i += self->vec_num;
    }
    return Vector_item(self, i);
  }
  if (PySlice_Check(item)) {
    Py_ssize_t start, stop, step, slicelength;

    if (PySlice_GetIndicesEx(item, self->vec_num, &start, &stop, &step, &slicelength) < 0) {
      return NULL;
    }

    if (slicelength <= 0) {
      return PyTuple_New(0);
    }
    if (step == 1) {
      return Vector_slice(self, start, stop);
    }

    PyErr_SetString(PyExc_IndexError, "slice steps not supported with vectors");
    return NULL;
  }

  PyErr_Format(
      PyExc_TypeError, "vector indices must be integers, not %.200s", Py_TYPE(item)->tp_name);
  return NULL;
}

/** Sequence generic subscript (set): `object[...] = x`. */
static int Vector_ass_subscript(VectorObject *self, PyObject *item, PyObject *value)
{
  if (PyIndex_Check(item)) {
    Py_ssize_t i = PyNumber_AsSsize_t(item, PyExc_IndexError);
    if (i == -1 && PyErr_Occurred()) {
      return -1;
    }
    if (i < 0) {
      i += self->vec_num;
    }
    return Vector_ass_item(self, i, value);
  }
  if (PySlice_Check(item)) {
    Py_ssize_t start, stop, step, slicelength;

    if (PySlice_GetIndicesEx(item, self->vec_num, &start, &stop, &step, &slicelength) < 0) {
      return -1;
    }

    if (step == 1) {
      return Vector_ass_slice(self, start, stop, value);
    }

    PyErr_SetString(PyExc_IndexError, "slice steps not supported with vectors");
    return -1;
  }

  PyErr_Format(
      PyExc_TypeError, "vector indices must be integers, not %.200s", Py_TYPE(item)->tp_name);
  return -1;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vector Type: Numeric Protocol Implementation
 * \{ */

/** Addition: `object + object`. */
static PyObject *Vector_add(PyObject *v1, PyObject *v2)
{
  VectorObject *vec1 = NULL, *vec2 = NULL;
  float *vec = NULL;

  if (!VectorObject_Check(v1) || !VectorObject_Check(v2)) {
    PyErr_Format(PyExc_AttributeError,
                 "Vector addition: (%s + %s) "
                 "invalid type for this operation",
                 Py_TYPE(v1)->tp_name,
                 Py_TYPE(v2)->tp_name);
    return NULL;
  }
  vec1 = (VectorObject *)v1;
  vec2 = (VectorObject *)v2;

  if (BaseMath_ReadCallback(vec1) == -1 || BaseMath_ReadCallback(vec2) == -1) {
    return NULL;
  }

  /* VECTOR + VECTOR. */
  if (vec1->vec_num != vec2->vec_num) {
    PyErr_SetString(PyExc_AttributeError,
                    "Vector addition: "
                    "vectors must have the same dimensions for this operation");
    return NULL;
  }

  vec = PyMem_Malloc(vec1->vec_num * sizeof(float));
  if (vec == NULL) {
    PyErr_SetString(PyExc_MemoryError,
                    "Vector(): "
                    "problem allocating pointer space");
    return NULL;
  }

  add_vn_vnvn(vec, vec1->vec, vec2->vec, vec1->vec_num);

  return Vector_CreatePyObject_alloc(vec, vec1->vec_num, Py_TYPE(v1));
}

/** Addition in-place: `object += object`. */
static PyObject *Vector_iadd(PyObject *v1, PyObject *v2)
{
  VectorObject *vec1 = NULL, *vec2 = NULL;

  if (!VectorObject_Check(v1) || !VectorObject_Check(v2)) {
    PyErr_Format(PyExc_AttributeError,
                 "Vector addition: (%s += %s) "
                 "invalid type for this operation",
                 Py_TYPE(v1)->tp_name,
                 Py_TYPE(v2)->tp_name);
    return NULL;
  }
  vec1 = (VectorObject *)v1;
  vec2 = (VectorObject *)v2;

  if (vec1->vec_num != vec2->vec_num) {
    PyErr_SetString(PyExc_AttributeError,
                    "Vector addition: "
                    "vectors must have the same dimensions for this operation");
    return NULL;
  }

  if (BaseMath_ReadCallback_ForWrite(vec1) == -1 || BaseMath_ReadCallback(vec2) == -1) {
    return NULL;
  }

  add_vn_vn(vec1->vec, vec2->vec, vec1->vec_num);

  (void)BaseMath_WriteCallback(vec1);
  Py_INCREF(v1);
  return v1;
}

/** Subtraction: `object - object`. */
static PyObject *Vector_sub(PyObject *v1, PyObject *v2)
{
  VectorObject *vec1 = NULL, *vec2 = NULL;
  float *vec;

  if (!VectorObject_Check(v1) || !VectorObject_Check(v2)) {
    PyErr_Format(PyExc_AttributeError,
                 "Vector subtraction: (%s - %s) "
                 "invalid type for this operation",
                 Py_TYPE(v1)->tp_name,
                 Py_TYPE(v2)->tp_name);
    return NULL;
  }
  vec1 = (VectorObject *)v1;
  vec2 = (VectorObject *)v2;

  if (BaseMath_ReadCallback(vec1) == -1 || BaseMath_ReadCallback(vec2) == -1) {
    return NULL;
  }

  if (vec1->vec_num != vec2->vec_num) {
    PyErr_SetString(PyExc_AttributeError,
                    "Vector subtraction: "
                    "vectors must have the same dimensions for this operation");
    return NULL;
  }

  vec = PyMem_Malloc(vec1->vec_num * sizeof(float));
  if (vec == NULL) {
    PyErr_SetString(PyExc_MemoryError,
                    "Vector(): "
                    "problem allocating pointer space");
    return NULL;
  }

  sub_vn_vnvn(vec, vec1->vec, vec2->vec, vec1->vec_num);

  return Vector_CreatePyObject_alloc(vec, vec1->vec_num, Py_TYPE(v1));
}

/** Subtraction in-place: `object -= object`. */
static PyObject *Vector_isub(PyObject *v1, PyObject *v2)
{
  VectorObject *vec1 = NULL, *vec2 = NULL;

  if (!VectorObject_Check(v1) || !VectorObject_Check(v2)) {
    PyErr_Format(PyExc_AttributeError,
                 "Vector subtraction: (%s -= %s) "
                 "invalid type for this operation",
                 Py_TYPE(v1)->tp_name,
                 Py_TYPE(v2)->tp_name);
    return NULL;
  }
  vec1 = (VectorObject *)v1;
  vec2 = (VectorObject *)v2;

  if (vec1->vec_num != vec2->vec_num) {
    PyErr_SetString(PyExc_AttributeError,
                    "Vector subtraction: "
                    "vectors must have the same dimensions for this operation");
    return NULL;
  }

  if (BaseMath_ReadCallback_ForWrite(vec1) == -1 || BaseMath_ReadCallback(vec2) == -1) {
    return NULL;
  }

  sub_vn_vn(vec1->vec, vec2->vec, vec1->vec_num);

  (void)BaseMath_WriteCallback(vec1);
  Py_INCREF(v1);
  return v1;
}

/* Multiply internal implementation `object * object`, `object *= object`. */

int column_vector_multiplication(float r_vec[MAX_DIMENSIONS], VectorObject *vec, MatrixObject *mat)
{
  float vec_cpy[MAX_DIMENSIONS];
  int row, col, z = 0;

  if (mat->col_num != vec->vec_num) {
    if (mat->col_num == 4 && vec->vec_num == 3) {
      vec_cpy[3] = 1.0f;
    }
    else {
      PyErr_SetString(PyExc_ValueError,
                      "matrix * vector: "
                      "len(matrix.col) and len(vector) must be the same, "
                      "except for 4x4 matrix * 3D vector.");
      return -1;
    }
  }

  memcpy(vec_cpy, vec->vec, vec->vec_num * sizeof(float));

  r_vec[3] = 1.0f;

  for (row = 0; row < mat->row_num; row++) {
    double dot = 0.0f;
    for (col = 0; col < mat->col_num; col++) {
      dot += (double)(MATRIX_ITEM(mat, row, col) * vec_cpy[col]);
    }
    r_vec[z++] = (float)dot;
  }

  return 0;
}

static PyObject *vector_mul_float(VectorObject *vec, const float scalar)
{
  float *tvec = PyMem_Malloc(vec->vec_num * sizeof(float));
  if (tvec == NULL) {
    PyErr_SetString(PyExc_MemoryError,
                    "vec * float: "
                    "problem allocating pointer space");
    return NULL;
  }

  mul_vn_vn_fl(tvec, vec->vec, vec->vec_num, scalar);
  return Vector_CreatePyObject_alloc(tvec, vec->vec_num, Py_TYPE(vec));
}

static PyObject *vector_mul_vec(VectorObject *vec1, VectorObject *vec2)
{
  float *tvec = PyMem_Malloc(vec1->vec_num * sizeof(float));
  if (tvec == NULL) {
    PyErr_SetString(PyExc_MemoryError,
                    "vec * vec: "
                    "problem allocating pointer space");
    return NULL;
  }

  mul_vn_vnvn(tvec, vec1->vec, vec2->vec, vec1->vec_num);
  return Vector_CreatePyObject_alloc(tvec, vec1->vec_num, Py_TYPE(vec1));
}

/** Multiplication (element-wise or scalar): `object * object`. */
static PyObject *Vector_mul(PyObject *v1, PyObject *v2)
{
  VectorObject *vec1 = NULL, *vec2 = NULL;
  float scalar;

  if (VectorObject_Check(v1)) {
    vec1 = (VectorObject *)v1;
    if (BaseMath_ReadCallback(vec1) == -1) {
      return NULL;
    }
  }
  if (VectorObject_Check(v2)) {
    vec2 = (VectorObject *)v2;
    if (BaseMath_ReadCallback(vec2) == -1) {
      return NULL;
    }
  }

  /* Intentionally don't support (Quaternion) here, uses reverse order instead. */

  /* make sure v1 is always the vector */
  if (vec1 && vec2) {
    if (vec1->vec_num != vec2->vec_num) {
      PyErr_SetString(PyExc_ValueError,
                      "Vector multiplication: "
                      "vectors must have the same dimensions for this operation");
      return NULL;
    }

    /* element-wise product */
    return vector_mul_vec(vec1, vec2);
  }
  if (vec1) {
    if (((scalar = PyFloat_AsDouble(v2)) == -1.0f && PyErr_Occurred()) == 0) { /* VEC * FLOAT */
      return vector_mul_float(vec1, scalar);
    }
  }
  else if (vec2) {
    if (((scalar = PyFloat_AsDouble(v1)) == -1.0f && PyErr_Occurred()) == 0) { /* FLOAT * VEC */
      return vector_mul_float(vec2, scalar);
    }
  }

  PyErr_Format(PyExc_TypeError,
               "Element-wise multiplication: "
               "not supported between '%.200s' and '%.200s' types",
               Py_TYPE(v1)->tp_name,
               Py_TYPE(v2)->tp_name);
  return NULL;
}

/** Multiplication in-place (element-wise or scalar): `object *= object`. */
static PyObject *Vector_imul(PyObject *v1, PyObject *v2)
{
  VectorObject *vec1 = NULL, *vec2 = NULL;
  float scalar;

  if (VectorObject_Check(v1)) {
    vec1 = (VectorObject *)v1;
    if (BaseMath_ReadCallback(vec1) == -1) {
      return NULL;
    }
  }
  if (VectorObject_Check(v2)) {
    vec2 = (VectorObject *)v2;
    if (BaseMath_ReadCallback(vec2) == -1) {
      return NULL;
    }
  }

  if (BaseMath_ReadCallback_ForWrite(vec1) == -1) {
    return NULL;
  }

  /* Intentionally don't support (Quaternion, Matrix) here, uses reverse order instead. */

  if (vec1 && vec2) {
    if (vec1->vec_num != vec2->vec_num) {
      PyErr_SetString(PyExc_ValueError,
                      "Vector multiplication: "
                      "vectors must have the same dimensions for this operation");
      return NULL;
    }

    /* Element-wise product in-place. */
    mul_vn_vn(vec1->vec, vec2->vec, vec1->vec_num);
  }
  else if (vec1 && (((scalar = PyFloat_AsDouble(v2)) == -1.0f && PyErr_Occurred()) ==
                    0)) { /* VEC *= FLOAT */
    mul_vn_fl(vec1->vec, vec1->vec_num, scalar);
  }
  else {
    PyErr_Format(PyExc_TypeError,
                 "In place element-wise multiplication: "
                 "not supported between '%.200s' and '%.200s' types",
                 Py_TYPE(v1)->tp_name,
                 Py_TYPE(v2)->tp_name);
    return NULL;
  }

  (void)BaseMath_WriteCallback(vec1);
  Py_INCREF(v1);
  return v1;
}

/** Multiplication (matrix multiply): `object @ object`. */
static PyObject *Vector_matmul(PyObject *v1, PyObject *v2)
{
  VectorObject *vec1 = NULL, *vec2 = NULL;
  int vec_num;

  if (VectorObject_Check(v1)) {
    vec1 = (VectorObject *)v1;
    if (BaseMath_ReadCallback(vec1) == -1) {
      return NULL;
    }
  }
  if (VectorObject_Check(v2)) {
    vec2 = (VectorObject *)v2;
    if (BaseMath_ReadCallback(vec2) == -1) {
      return NULL;
    }
  }

  /* Intentionally don't support (Quaternion) here, uses reverse order instead. */

  /* make sure v1 is always the vector */
  if (vec1 && vec2) {
    if (vec1->vec_num != vec2->vec_num) {
      PyErr_SetString(PyExc_ValueError,
                      "Vector multiplication: "
                      "vectors must have the same dimensions for this operation");
      return NULL;
    }

    /* Dot product. */
    return PyFloat_FromDouble(dot_vn_vn(vec1->vec, vec2->vec, vec1->vec_num));
  }
  if (vec1) {
    if (MatrixObject_Check(v2)) {
      /* VEC @ MATRIX */
      float tvec[MAX_DIMENSIONS];

      if (BaseMath_ReadCallback((MatrixObject *)v2) == -1) {
        return NULL;
      }
      if (row_vector_multiplication(tvec, vec1, (MatrixObject *)v2) == -1) {
        return NULL;
      }

      if (((MatrixObject *)v2)->row_num == 4 && vec1->vec_num == 3) {
        vec_num = 3;
      }
      else {
        vec_num = ((MatrixObject *)v2)->col_num;
      }

      return Vector_CreatePyObject(tvec, vec_num, Py_TYPE(vec1));
    }
  }

  PyErr_Format(PyExc_TypeError,
               "Vector multiplication: "
               "not supported between '%.200s' and '%.200s' types",
               Py_TYPE(v1)->tp_name,
               Py_TYPE(v2)->tp_name);
  return NULL;
}

/** Multiplication in-place (matrix multiply): `object @= object`. */
static PyObject *Vector_imatmul(PyObject *v1, PyObject *v2)
{
  PyErr_Format(PyExc_TypeError,
               "In place vector multiplication: "
               "not supported between '%.200s' and '%.200s' types",
               Py_TYPE(v1)->tp_name,
               Py_TYPE(v2)->tp_name);
  return NULL;
}

/** Division: `object / object`. */
static PyObject *Vector_div(PyObject *v1, PyObject *v2)
{
  float *vec = NULL, scalar;
  VectorObject *vec1 = NULL;

  if (!VectorObject_Check(v1)) { /* not a vector */
    PyErr_SetString(PyExc_TypeError,
                    "Vector division: "
                    "Vector must be divided by a float");
    return NULL;
  }
  vec1 = (VectorObject *)v1; /* vector */

  if (BaseMath_ReadCallback(vec1) == -1) {
    return NULL;
  }

  if ((scalar = PyFloat_AsDouble(v2)) == -1.0f && PyErr_Occurred()) {
    /* parsed item not a number */
    PyErr_SetString(PyExc_TypeError,
                    "Vector division: "
                    "Vector must be divided by a float");
    return NULL;
  }

  if (scalar == 0.0f) {
    PyErr_SetString(PyExc_ZeroDivisionError,
                    "Vector division: "
                    "divide by zero error");
    return NULL;
  }

  vec = PyMem_Malloc(vec1->vec_num * sizeof(float));

  if (vec == NULL) {
    PyErr_SetString(PyExc_MemoryError,
                    "vec / value: "
                    "problem allocating pointer space");
    return NULL;
  }

  mul_vn_vn_fl(vec, vec1->vec, vec1->vec_num, 1.0f / scalar);

  return Vector_CreatePyObject_alloc(vec, vec1->vec_num, Py_TYPE(v1));
}

/** Division in-place: `object /= object`. */
static PyObject *Vector_idiv(PyObject *v1, PyObject *v2)
{
  float scalar;
  VectorObject *vec1 = (VectorObject *)v1;

  if (BaseMath_ReadCallback_ForWrite(vec1) == -1) {
    return NULL;
  }

  if ((scalar = PyFloat_AsDouble(v2)) == -1.0f && PyErr_Occurred()) {
    /* parsed item not a number */
    PyErr_SetString(PyExc_TypeError,
                    "Vector division: "
                    "Vector must be divided by a float");
    return NULL;
  }

  if (scalar == 0.0f) {
    PyErr_SetString(PyExc_ZeroDivisionError,
                    "Vector division: "
                    "divide by zero error");
    return NULL;
  }

  mul_vn_fl(vec1->vec, vec1->vec_num, 1.0f / scalar);

  (void)BaseMath_WriteCallback(vec1);

  Py_INCREF(v1);
  return v1;
}

/** Negative (returns the negative of this object): `-object`. */
static PyObject *Vector_neg(VectorObject *self)
{
  float *tvec;

  if (BaseMath_ReadCallback(self) == -1) {
    return NULL;
  }

  tvec = PyMem_Malloc(self->vec_num * sizeof(float));
  negate_vn_vn(tvec, self->vec, self->vec_num);
  return Vector_CreatePyObject_alloc(tvec, self->vec_num, Py_TYPE(self));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vector Type: Protocol Declarations
 * \{ */

static PySequenceMethods Vector_SeqMethods = {
    (lenfunc)Vector_len,              /*sq_length*/
    (binaryfunc)NULL,                 /*sq_concat*/
    (ssizeargfunc)NULL,               /*sq_repeat*/
    (ssizeargfunc)Vector_item,        /*sq_item*/
    NULL,                             /*sq_slice(DEPRECATED)*/
    (ssizeobjargproc)Vector_ass_item, /*sq_ass_item*/
    NULL,                             /*sq_ass_slice(DEPRECATED)*/
    (objobjproc)NULL,                 /*sq_contains*/
    (binaryfunc)NULL,                 /*sq_inplace_concat */
    (ssizeargfunc)NULL,               /*sq_inplace_repeat */
};

static PyMappingMethods Vector_AsMapping = {
    (lenfunc)Vector_len,
    (binaryfunc)Vector_subscript,
    (objobjargproc)Vector_ass_subscript,
};

static PyNumberMethods Vector_NumMethods = {
    (binaryfunc)Vector_add,     /*nb_add*/
    (binaryfunc)Vector_sub,     /*nb_subtract*/
    (binaryfunc)Vector_mul,     /*nb_multiply*/
    NULL,                       /*nb_remainder*/
    NULL,                       /*nb_divmod*/
    NULL,                       /*nb_power*/
    (unaryfunc)Vector_neg,      /*nb_negative*/
    (unaryfunc)Vector_copy,     /*tp_positive*/
    (unaryfunc)NULL,            /*tp_absolute*/
    (inquiry)NULL,              /*tp_bool*/
    (unaryfunc)NULL,            /*nb_invert*/
    NULL,                       /*nb_lshift*/
    (binaryfunc)NULL,           /*nb_rshift*/
    NULL,                       /*nb_and*/
    NULL,                       /*nb_xor*/
    NULL,                       /*nb_or*/
    NULL,                       /*nb_int*/
    NULL,                       /*nb_reserved*/
    NULL,                       /*nb_float*/
    Vector_iadd,                /*nb_inplace_add*/
    Vector_isub,                /*nb_inplace_subtract*/
    Vector_imul,                /*nb_inplace_multiply*/
    NULL,                       /*nb_inplace_remainder*/
    NULL,                       /*nb_inplace_power*/
    NULL,                       /*nb_inplace_lshift*/
    NULL,                       /*nb_inplace_rshift*/
    NULL,                       /*nb_inplace_and*/
    NULL,                       /*nb_inplace_xor*/
    NULL,                       /*nb_inplace_or*/
    NULL,                       /*nb_floor_divide*/
    Vector_div,                 /*nb_true_divide*/
    NULL,                       /*nb_inplace_floor_divide*/
    Vector_idiv,                /*nb_inplace_true_divide*/
    NULL,                       /*nb_index*/
    (binaryfunc)Vector_matmul,  /*nb_matrix_multiply*/
    (binaryfunc)Vector_imatmul, /*nb_inplace_matrix_multiply*/
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vector Type: Get/Set Item Implementation
 * \{ */

/* Vector axis: `vector.x/y/z/w`. */

PyDoc_STRVAR(Vector_axis_x_doc, "Vector X axis.\n\n:type: float");
PyDoc_STRVAR(Vector_axis_y_doc, "Vector Y axis.\n\n:type: float");
PyDoc_STRVAR(Vector_axis_z_doc, "Vector Z axis (3D Vectors only).\n\n:type: float");
PyDoc_STRVAR(Vector_axis_w_doc, "Vector W axis (4D Vectors only).\n\n:type: float");

static PyObject *Vector_axis_get(VectorObject *self, void *type)
{
  return vector_item_internal(self, POINTER_AS_INT(type), true);
}

static int Vector_axis_set(VectorObject *self, PyObject *value, void *type)
{
  return vector_ass_item_internal(self, POINTER_AS_INT(type), value, true);
}

/* `Vector.length`. */

PyDoc_STRVAR(Vector_length_doc, "Vector Length.\n\n:type: float");
static PyObject *Vector_length_get(VectorObject *self, void *UNUSED(closure))
{
  if (BaseMath_ReadCallback(self) == -1) {
    return NULL;
  }

  return PyFloat_FromDouble(sqrt(dot_vn_vn(self->vec, self->vec, self->vec_num)));
}

static int Vector_length_set(VectorObject *self, PyObject *value)
{
  double dot = 0.0f, param;

  if (BaseMath_ReadCallback_ForWrite(self) == -1) {
    return -1;
  }

  if ((param = PyFloat_AsDouble(value)) == -1.0 && PyErr_Occurred()) {
    PyErr_SetString(PyExc_TypeError, "length must be set to a number");
    return -1;
  }

  if (param < 0.0) {
    PyErr_SetString(PyExc_ValueError, "cannot set a vectors length to a negative value");
    return -1;
  }
  if (param == 0.0) {
    copy_vn_fl(self->vec, self->vec_num, 0.0f);
    return 0;
  }

  dot = dot_vn_vn(self->vec, self->vec, self->vec_num);

  if (!dot) {
    /* can't sqrt zero */
    return 0;
  }

  dot = sqrt(dot);

  if (dot == param) {
    return 0;
  }

  dot = dot / param;

  mul_vn_fl(self->vec, self->vec_num, 1.0 / dot);

  (void)BaseMath_WriteCallback(self); /* checked already */

  return 0;
}

/* `Vector.length_squared`. */
PyDoc_STRVAR(Vector_length_squared_doc, "Vector length squared (v.dot(v)).\n\n:type: float");
static PyObject *Vector_length_squared_get(VectorObject *self, void *UNUSED(closure))
{
  if (BaseMath_ReadCallback(self) == -1) {
    return NULL;
  }

  return PyFloat_FromDouble(dot_vn_vn(self->vec, self->vec, self->vec_num));
}

/**
 * Python script used to make swizzle array:
 *
 * \code{.py}
 * SWIZZLE_BITS_PER_AXIS = 3
 * SWIZZLE_VALID_AXIS = 0x4
 *
 * axis_dict = {}
 * axis_pos = {'x': 0, 'y': 1, 'z': 2, 'w': 3}
 * axis_chars = 'xyzw'
 * while len(axis_chars) >= 2:
 *     for axis_0 in axis_chars:
 *         axis_0_pos = axis_pos[axis_0]
 *         for axis_1 in axis_chars:
 *             axis_1_pos = axis_pos[axis_1]
 *             axis_dict[axis_0 + axis_1] = (
 *                 '((%s | SWIZZLE_VALID_AXIS) | '
 *                 '((%s | SWIZZLE_VALID_AXIS) << SWIZZLE_BITS_PER_AXIS))' %
 *                 (axis_0_pos, axis_1_pos))
 *             if len(axis_chars) > 2:
 *                 for axis_2 in axis_chars:
 *                     axis_2_pos = axis_pos[axis_2]
 *                     axis_dict[axis_0 + axis_1 + axis_2] = (
 *                         '((%s | SWIZZLE_VALID_AXIS) | '
 *                         '((%s | SWIZZLE_VALID_AXIS) << SWIZZLE_BITS_PER_AXIS) | '
 *                         '((%s | SWIZZLE_VALID_AXIS) << (SWIZZLE_BITS_PER_AXIS * 2)))' %
 *                         (axis_0_pos, axis_1_pos, axis_2_pos))
 *                     if len(axis_chars) > 3:
 *                         for axis_3 in axis_chars:
 *                             axis_3_pos = axis_pos[axis_3]
 *                             axis_dict[axis_0 + axis_1 + axis_2 + axis_3] = (
 *                                 '((%s | SWIZZLE_VALID_AXIS) | '
 *                                 '((%s | SWIZZLE_VALID_AXIS) << SWIZZLE_BITS_PER_AXIS) | '
 *                                 '((%s | SWIZZLE_VALID_AXIS) << (SWIZZLE_BITS_PER_AXIS * 2)) | '
 *                                 '((%s | SWIZZLE_VALID_AXIS) << (SWIZZLE_BITS_PER_AXIS * 3)))  '
 *                                 %
 *                                 (axis_0_pos, axis_1_pos, axis_2_pos, axis_3_pos))
 *
 *     axis_chars = axis_chars[:-1]
 * items = list(axis_dict.items())
 * items.sort(
 *     key=lambda a: a[0].replace('x', '0').replace('y', '1').replace('z', '2').replace('w', '3')
 * )
 *
 * unique = set()
 * for key, val in items:
 *     num = eval(val)
 *     set_str = 'Vector_swizzle_set' if (len(set(key)) == len(key)) else 'NULL'
 *     key_args = ', '.join(["'%s'" % c for c in key.upper()])
 *     print('\t{"%s", %s(getter)Vector_swizzle_get, (setter)%s, NULL, SWIZZLE%d(%s)},' %
 *           (key, (' ' * (4 - len(key))), set_str, len(key), key_args))
 *     unique.add(num)
 *
 * if len(unique) != len(items):
 *     print("ERROR, duplicate values found")
 * \endcode
 */

/**
 * Get a new Vector according to the provided swizzle bits.
 */
static PyObject *Vector_swizzle_get(VectorObject *self, void *closure)
{
  size_t axis_to;
  size_t axis_from;
  float vec[MAX_DIMENSIONS];
  uint swizzleClosure;

  if (BaseMath_ReadCallback(self) == -1) {
    return NULL;
  }

  /* Unpack the axes from the closure into an array. */
  axis_to = 0;
  swizzleClosure = POINTER_AS_INT(closure);
  while (swizzleClosure & SWIZZLE_VALID_AXIS) {
    axis_from = swizzleClosure & SWIZZLE_AXIS;
    if (axis_from >= self->vec_num) {
      PyErr_SetString(PyExc_AttributeError,
                      "Vector swizzle: "
                      "specified axis not present");
      return NULL;
    }

    vec[axis_to] = self->vec[axis_from];
    swizzleClosure = swizzleClosure >> SWIZZLE_BITS_PER_AXIS;
    axis_to++;
  }

  return Vector_CreatePyObject(vec, axis_to, Py_TYPE(self));
}

/**
 * Set the items of this vector using a swizzle.
 * - If value is a vector or list this operates like an array copy, except that
 *   the destination is effectively re-ordered as defined by the swizzle. At
 *   most min(len(source), len(dest)) values will be copied.
 * - If the value is scalar, it is copied to all axes listed in the swizzle.
 * - If an axis appears more than once in the swizzle, the final occurrence is
 *   the one that determines its value.
 *
 * \return 0 on success and -1 on failure. On failure, the vector will be unchanged.
 */
static int Vector_swizzle_set(VectorObject *self, PyObject *value, void *closure)
{
  size_t size_from;
  float scalarVal;

  size_t axis_from;
  size_t axis_to;

  uint swizzleClosure;

  float tvec[MAX_DIMENSIONS];
  float vec_assign[MAX_DIMENSIONS];

  if (BaseMath_ReadCallback_ForWrite(self) == -1) {
    return -1;
  }

  /* Check that the closure can be used with this vector: even 2D vectors have
   * swizzles defined for axes z and w, but they would be invalid. */
  swizzleClosure = POINTER_AS_INT(closure);
  axis_from = 0;

  while (swizzleClosure & SWIZZLE_VALID_AXIS) {
    axis_to = swizzleClosure & SWIZZLE_AXIS;
    if (axis_to >= self->vec_num) {
      PyErr_SetString(PyExc_AttributeError,
                      "Vector swizzle: "
                      "specified axis not present");
      return -1;
    }
    swizzleClosure = swizzleClosure >> SWIZZLE_BITS_PER_AXIS;
    axis_from++;
  }

  if (((scalarVal = PyFloat_AsDouble(value)) == -1 && PyErr_Occurred()) == 0) {
    int i;

    for (i = 0; i < MAX_DIMENSIONS; i++) {
      vec_assign[i] = scalarVal;
    }

    size_from = axis_from;
  }
  else if (((void)PyErr_Clear()), /* run but ignore the result */
           (size_from = mathutils_array_parse(
                vec_assign, 2, 4, value, "mathutils.Vector.**** = swizzle assignment")) == -1) {
    return -1;
  }

  if (axis_from != size_from) {
    PyErr_SetString(PyExc_AttributeError, "Vector swizzle: size does not match swizzle");
    return -1;
  }

  /* Copy vector contents onto swizzled axes. */
  axis_from = 0;
  swizzleClosure = POINTER_AS_INT(closure);

  /* We must first copy current vec into tvec, else some org values may be lost.
   * See T31760.
   * Assuming self->vec_num can't be higher than MAX_DIMENSIONS! */
  memcpy(tvec, self->vec, self->vec_num * sizeof(float));

  while (swizzleClosure & SWIZZLE_VALID_AXIS) {
    axis_to = swizzleClosure & SWIZZLE_AXIS;
    tvec[axis_to] = vec_assign[axis_from];
    swizzleClosure = swizzleClosure >> SWIZZLE_BITS_PER_AXIS;
    axis_from++;
  }

  /* We must copy back the whole tvec into vec, else some changes may be lost (e.g. xz...).
   * See T31760. */
  memcpy(self->vec, tvec, self->vec_num * sizeof(float));
  /* continue with BaseMathObject_WriteCallback at the end */

  if (BaseMath_WriteCallback(self) == -1) {
    return -1;
  }

  return 0;
}

#define _SWIZZLE1(a) ((a) | SWIZZLE_VALID_AXIS)
#define _SWIZZLE2(a, b) (_SWIZZLE1(a) | (((b) | SWIZZLE_VALID_AXIS) << (SWIZZLE_BITS_PER_AXIS)))
#define _SWIZZLE3(a, b, c) \
  (_SWIZZLE2(a, b) | (((c) | SWIZZLE_VALID_AXIS) << (SWIZZLE_BITS_PER_AXIS * 2)))
#define _SWIZZLE4(a, b, c, d) \
  (_SWIZZLE3(a, b, c) | (((d) | SWIZZLE_VALID_AXIS) << (SWIZZLE_BITS_PER_AXIS * 3)))

#define SWIZZLE2(a, b) POINTER_FROM_INT(_SWIZZLE2(a, b))
#define SWIZZLE3(a, b, c) POINTER_FROM_INT(_SWIZZLE3(a, b, c))
#define SWIZZLE4(a, b, c, d) POINTER_FROM_INT(_SWIZZLE4(a, b, c, d))

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vector Type: Get/Set Item Definitions
 * \{ */

static PyGetSetDef Vector_getseters[] = {
    {"x", (getter)Vector_axis_get, (setter)Vector_axis_set, Vector_axis_x_doc, (void *)0},
    {"y", (getter)Vector_axis_get, (setter)Vector_axis_set, Vector_axis_y_doc, (void *)1},
    {"z", (getter)Vector_axis_get, (setter)Vector_axis_set, Vector_axis_z_doc, (void *)2},
    {"w", (getter)Vector_axis_get, (setter)Vector_axis_set, Vector_axis_w_doc, (void *)3},
    {"length", (getter)Vector_length_get, (setter)Vector_length_set, Vector_length_doc, NULL},
    {"length_squared",
     (getter)Vector_length_squared_get,
     (setter)NULL,
     Vector_length_squared_doc,
     NULL},
    {"magnitude", (getter)Vector_length_get, (setter)Vector_length_set, Vector_length_doc, NULL},
    {"is_wrapped",
     (getter)BaseMathObject_is_wrapped_get,
     (setter)NULL,
     BaseMathObject_is_wrapped_doc,
     NULL},
    {"is_frozen",
     (getter)BaseMathObject_is_frozen_get,
     (setter)NULL,
     BaseMathObject_is_frozen_doc,
     NULL},
    {"is_valid",
     (getter)BaseMathObject_is_valid_get,
     (setter)NULL,
     BaseMathObject_is_valid_doc,
     NULL},
    {"owner", (getter)BaseMathObject_owner_get, (setter)NULL, BaseMathObject_owner_doc, NULL},

    /* Auto-generated swizzle attributes, see Python script above. */
    {"xx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE2(0, 0)},
    {"xxx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE3(0, 0, 0)},
    {"xxxx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 0, 0, 0)},
    {"xxxy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 0, 0, 1)},
    {"xxxz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 0, 0, 2)},
    {"xxxw", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 0, 0, 3)},
    {"xxy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE3(0, 0, 1)},
    {"xxyx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 0, 1, 0)},
    {"xxyy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 0, 1, 1)},
    {"xxyz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 0, 1, 2)},
    {"xxyw", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 0, 1, 3)},
    {"xxz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE3(0, 0, 2)},
    {"xxzx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 0, 2, 0)},
    {"xxzy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 0, 2, 1)},
    {"xxzz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 0, 2, 2)},
    {"xxzw", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 0, 2, 3)},
    {"xxw", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE3(0, 0, 3)},
    {"xxwx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 0, 3, 0)},
    {"xxwy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 0, 3, 1)},
    {"xxwz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 0, 3, 2)},
    {"xxww", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 0, 3, 3)},
    {"xy", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE2(0, 1)},
    {"xyx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE3(0, 1, 0)},
    {"xyxx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 1, 0, 0)},
    {"xyxy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 1, 0, 1)},
    {"xyxz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 1, 0, 2)},
    {"xyxw", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 1, 0, 3)},
    {"xyy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE3(0, 1, 1)},
    {"xyyx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 1, 1, 0)},
    {"xyyy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 1, 1, 1)},
    {"xyyz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 1, 1, 2)},
    {"xyyw", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 1, 1, 3)},
    {"xyz", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE3(0, 1, 2)},
    {"xyzx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 1, 2, 0)},
    {"xyzy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 1, 2, 1)},
    {"xyzz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 1, 2, 2)},
    {"xyzw", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE4(0, 1, 2, 3)},
    {"xyw", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE3(0, 1, 3)},
    {"xywx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 1, 3, 0)},
    {"xywy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 1, 3, 1)},
    {"xywz", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE4(0, 1, 3, 2)},
    {"xyww", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 1, 3, 3)},
    {"xz", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE2(0, 2)},
    {"xzx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE3(0, 2, 0)},
    {"xzxx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 2, 0, 0)},
    {"xzxy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 2, 0, 1)},
    {"xzxz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 2, 0, 2)},
    {"xzxw", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 2, 0, 3)},
    {"xzy", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE3(0, 2, 1)},
    {"xzyx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 2, 1, 0)},
    {"xzyy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 2, 1, 1)},
    {"xzyz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 2, 1, 2)},
    {"xzyw", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE4(0, 2, 1, 3)},
    {"xzz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE3(0, 2, 2)},
    {"xzzx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 2, 2, 0)},
    {"xzzy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 2, 2, 1)},
    {"xzzz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 2, 2, 2)},
    {"xzzw", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 2, 2, 3)},
    {"xzw", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE3(0, 2, 3)},
    {"xzwx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 2, 3, 0)},
    {"xzwy", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE4(0, 2, 3, 1)},
    {"xzwz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 2, 3, 2)},
    {"xzww", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 2, 3, 3)},
    {"xw", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE2(0, 3)},
    {"xwx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE3(0, 3, 0)},
    {"xwxx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 3, 0, 0)},
    {"xwxy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 3, 0, 1)},
    {"xwxz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 3, 0, 2)},
    {"xwxw", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 3, 0, 3)},
    {"xwy", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE3(0, 3, 1)},
    {"xwyx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 3, 1, 0)},
    {"xwyy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 3, 1, 1)},
    {"xwyz", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE4(0, 3, 1, 2)},
    {"xwyw", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 3, 1, 3)},
    {"xwz", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE3(0, 3, 2)},
    {"xwzx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 3, 2, 0)},
    {"xwzy", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE4(0, 3, 2, 1)},
    {"xwzz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 3, 2, 2)},
    {"xwzw", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 3, 2, 3)},
    {"xww", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE3(0, 3, 3)},
    {"xwwx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 3, 3, 0)},
    {"xwwy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 3, 3, 1)},
    {"xwwz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 3, 3, 2)},
    {"xwww", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(0, 3, 3, 3)},
    {"yx", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE2(1, 0)},
    {"yxx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE3(1, 0, 0)},
    {"yxxx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 0, 0, 0)},
    {"yxxy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 0, 0, 1)},
    {"yxxz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 0, 0, 2)},
    {"yxxw", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 0, 0, 3)},
    {"yxy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE3(1, 0, 1)},
    {"yxyx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 0, 1, 0)},
    {"yxyy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 0, 1, 1)},
    {"yxyz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 0, 1, 2)},
    {"yxyw", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 0, 1, 3)},
    {"yxz", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE3(1, 0, 2)},
    {"yxzx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 0, 2, 0)},
    {"yxzy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 0, 2, 1)},
    {"yxzz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 0, 2, 2)},
    {"yxzw", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE4(1, 0, 2, 3)},
    {"yxw", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE3(1, 0, 3)},
    {"yxwx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 0, 3, 0)},
    {"yxwy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 0, 3, 1)},
    {"yxwz", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE4(1, 0, 3, 2)},
    {"yxww", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 0, 3, 3)},
    {"yy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE2(1, 1)},
    {"yyx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE3(1, 1, 0)},
    {"yyxx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 1, 0, 0)},
    {"yyxy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 1, 0, 1)},
    {"yyxz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 1, 0, 2)},
    {"yyxw", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 1, 0, 3)},
    {"yyy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE3(1, 1, 1)},
    {"yyyx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 1, 1, 0)},
    {"yyyy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 1, 1, 1)},
    {"yyyz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 1, 1, 2)},
    {"yyyw", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 1, 1, 3)},
    {"yyz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE3(1, 1, 2)},
    {"yyzx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 1, 2, 0)},
    {"yyzy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 1, 2, 1)},
    {"yyzz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 1, 2, 2)},
    {"yyzw", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 1, 2, 3)},
    {"yyw", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE3(1, 1, 3)},
    {"yywx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 1, 3, 0)},
    {"yywy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 1, 3, 1)},
    {"yywz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 1, 3, 2)},
    {"yyww", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 1, 3, 3)},
    {"yz", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE2(1, 2)},
    {"yzx", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE3(1, 2, 0)},
    {"yzxx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 2, 0, 0)},
    {"yzxy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 2, 0, 1)},
    {"yzxz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 2, 0, 2)},
    {"yzxw", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE4(1, 2, 0, 3)},
    {"yzy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE3(1, 2, 1)},
    {"yzyx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 2, 1, 0)},
    {"yzyy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 2, 1, 1)},
    {"yzyz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 2, 1, 2)},
    {"yzyw", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 2, 1, 3)},
    {"yzz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE3(1, 2, 2)},
    {"yzzx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 2, 2, 0)},
    {"yzzy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 2, 2, 1)},
    {"yzzz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 2, 2, 2)},
    {"yzzw", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 2, 2, 3)},
    {"yzw", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE3(1, 2, 3)},
    {"yzwx", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE4(1, 2, 3, 0)},
    {"yzwy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 2, 3, 1)},
    {"yzwz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 2, 3, 2)},
    {"yzww", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 2, 3, 3)},
    {"yw", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE2(1, 3)},
    {"ywx", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE3(1, 3, 0)},
    {"ywxx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 3, 0, 0)},
    {"ywxy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 3, 0, 1)},
    {"ywxz", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE4(1, 3, 0, 2)},
    {"ywxw", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 3, 0, 3)},
    {"ywy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE3(1, 3, 1)},
    {"ywyx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 3, 1, 0)},
    {"ywyy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 3, 1, 1)},
    {"ywyz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 3, 1, 2)},
    {"ywyw", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 3, 1, 3)},
    {"ywz", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE3(1, 3, 2)},
    {"ywzx", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE4(1, 3, 2, 0)},
    {"ywzy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 3, 2, 1)},
    {"ywzz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 3, 2, 2)},
    {"ywzw", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 3, 2, 3)},
    {"yww", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE3(1, 3, 3)},
    {"ywwx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 3, 3, 0)},
    {"ywwy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 3, 3, 1)},
    {"ywwz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 3, 3, 2)},
    {"ywww", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(1, 3, 3, 3)},
    {"zx", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE2(2, 0)},
    {"zxx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE3(2, 0, 0)},
    {"zxxx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 0, 0, 0)},
    {"zxxy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 0, 0, 1)},
    {"zxxz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 0, 0, 2)},
    {"zxxw", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 0, 0, 3)},
    {"zxy", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE3(2, 0, 1)},
    {"zxyx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 0, 1, 0)},
    {"zxyy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 0, 1, 1)},
    {"zxyz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 0, 1, 2)},
    {"zxyw", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE4(2, 0, 1, 3)},
    {"zxz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE3(2, 0, 2)},
    {"zxzx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 0, 2, 0)},
    {"zxzy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 0, 2, 1)},
    {"zxzz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 0, 2, 2)},
    {"zxzw", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 0, 2, 3)},
    {"zxw", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE3(2, 0, 3)},
    {"zxwx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 0, 3, 0)},
    {"zxwy", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE4(2, 0, 3, 1)},
    {"zxwz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 0, 3, 2)},
    {"zxww", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 0, 3, 3)},
    {"zy", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE2(2, 1)},
    {"zyx", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE3(2, 1, 0)},
    {"zyxx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 1, 0, 0)},
    {"zyxy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 1, 0, 1)},
    {"zyxz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 1, 0, 2)},
    {"zyxw", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE4(2, 1, 0, 3)},
    {"zyy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE3(2, 1, 1)},
    {"zyyx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 1, 1, 0)},
    {"zyyy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 1, 1, 1)},
    {"zyyz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 1, 1, 2)},
    {"zyyw", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 1, 1, 3)},
    {"zyz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE3(2, 1, 2)},
    {"zyzx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 1, 2, 0)},
    {"zyzy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 1, 2, 1)},
    {"zyzz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 1, 2, 2)},
    {"zyzw", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 1, 2, 3)},
    {"zyw", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE3(2, 1, 3)},
    {"zywx", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE4(2, 1, 3, 0)},
    {"zywy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 1, 3, 1)},
    {"zywz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 1, 3, 2)},
    {"zyww", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 1, 3, 3)},
    {"zz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE2(2, 2)},
    {"zzx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE3(2, 2, 0)},
    {"zzxx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 2, 0, 0)},
    {"zzxy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 2, 0, 1)},
    {"zzxz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 2, 0, 2)},
    {"zzxw", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 2, 0, 3)},
    {"zzy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE3(2, 2, 1)},
    {"zzyx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 2, 1, 0)},
    {"zzyy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 2, 1, 1)},
    {"zzyz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 2, 1, 2)},
    {"zzyw", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 2, 1, 3)},
    {"zzz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE3(2, 2, 2)},
    {"zzzx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 2, 2, 0)},
    {"zzzy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 2, 2, 1)},
    {"zzzz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 2, 2, 2)},
    {"zzzw", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 2, 2, 3)},
    {"zzw", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE3(2, 2, 3)},
    {"zzwx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 2, 3, 0)},
    {"zzwy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 2, 3, 1)},
    {"zzwz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 2, 3, 2)},
    {"zzww", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 2, 3, 3)},
    {"zw", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE2(2, 3)},
    {"zwx", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE3(2, 3, 0)},
    {"zwxx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 3, 0, 0)},
    {"zwxy", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE4(2, 3, 0, 1)},
    {"zwxz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 3, 0, 2)},
    {"zwxw", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 3, 0, 3)},
    {"zwy", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE3(2, 3, 1)},
    {"zwyx", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE4(2, 3, 1, 0)},
    {"zwyy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 3, 1, 1)},
    {"zwyz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 3, 1, 2)},
    {"zwyw", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 3, 1, 3)},
    {"zwz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE3(2, 3, 2)},
    {"zwzx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 3, 2, 0)},
    {"zwzy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 3, 2, 1)},
    {"zwzz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 3, 2, 2)},
    {"zwzw", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 3, 2, 3)},
    {"zww", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE3(2, 3, 3)},
    {"zwwx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 3, 3, 0)},
    {"zwwy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 3, 3, 1)},
    {"zwwz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 3, 3, 2)},
    {"zwww", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(2, 3, 3, 3)},
    {"wx", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE2(3, 0)},
    {"wxx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE3(3, 0, 0)},
    {"wxxx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 0, 0, 0)},
    {"wxxy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 0, 0, 1)},
    {"wxxz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 0, 0, 2)},
    {"wxxw", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 0, 0, 3)},
    {"wxy", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE3(3, 0, 1)},
    {"wxyx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 0, 1, 0)},
    {"wxyy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 0, 1, 1)},
    {"wxyz", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE4(3, 0, 1, 2)},
    {"wxyw", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 0, 1, 3)},
    {"wxz", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE3(3, 0, 2)},
    {"wxzx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 0, 2, 0)},
    {"wxzy", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE4(3, 0, 2, 1)},
    {"wxzz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 0, 2, 2)},
    {"wxzw", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 0, 2, 3)},
    {"wxw", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE3(3, 0, 3)},
    {"wxwx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 0, 3, 0)},
    {"wxwy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 0, 3, 1)},
    {"wxwz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 0, 3, 2)},
    {"wxww", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 0, 3, 3)},
    {"wy", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE2(3, 1)},
    {"wyx", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE3(3, 1, 0)},
    {"wyxx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 1, 0, 0)},
    {"wyxy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 1, 0, 1)},
    {"wyxz", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE4(3, 1, 0, 2)},
    {"wyxw", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 1, 0, 3)},
    {"wyy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE3(3, 1, 1)},
    {"wyyx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 1, 1, 0)},
    {"wyyy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 1, 1, 1)},
    {"wyyz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 1, 1, 2)},
    {"wyyw", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 1, 1, 3)},
    {"wyz", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE3(3, 1, 2)},
    {"wyzx", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE4(3, 1, 2, 0)},
    {"wyzy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 1, 2, 1)},
    {"wyzz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 1, 2, 2)},
    {"wyzw", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 1, 2, 3)},
    {"wyw", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE3(3, 1, 3)},
    {"wywx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 1, 3, 0)},
    {"wywy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 1, 3, 1)},
    {"wywz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 1, 3, 2)},
    {"wyww", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 1, 3, 3)},
    {"wz", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE2(3, 2)},
    {"wzx", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE3(3, 2, 0)},
    {"wzxx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 2, 0, 0)},
    {"wzxy", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE4(3, 2, 0, 1)},
    {"wzxz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 2, 0, 2)},
    {"wzxw", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 2, 0, 3)},
    {"wzy", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE3(3, 2, 1)},
    {"wzyx", (getter)Vector_swizzle_get, (setter)Vector_swizzle_set, NULL, SWIZZLE4(3, 2, 1, 0)},
    {"wzyy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 2, 1, 1)},
    {"wzyz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 2, 1, 2)},
    {"wzyw", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 2, 1, 3)},
    {"wzz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE3(3, 2, 2)},
    {"wzzx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 2, 2, 0)},
    {"wzzy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 2, 2, 1)},
    {"wzzz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 2, 2, 2)},
    {"wzzw", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 2, 2, 3)},
    {"wzw", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE3(3, 2, 3)},
    {"wzwx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 2, 3, 0)},
    {"wzwy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 2, 3, 1)},
    {"wzwz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 2, 3, 2)},
    {"wzww", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 2, 3, 3)},
    {"ww", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE2(3, 3)},
    {"wwx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE3(3, 3, 0)},
    {"wwxx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 3, 0, 0)},
    {"wwxy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 3, 0, 1)},
    {"wwxz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 3, 0, 2)},
    {"wwxw", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 3, 0, 3)},
    {"wwy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE3(3, 3, 1)},
    {"wwyx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 3, 1, 0)},
    {"wwyy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 3, 1, 1)},
    {"wwyz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 3, 1, 2)},
    {"wwyw", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 3, 1, 3)},
    {"wwz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE3(3, 3, 2)},
    {"wwzx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 3, 2, 0)},
    {"wwzy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 3, 2, 1)},
    {"wwzz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 3, 2, 2)},
    {"wwzw", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 3, 2, 3)},
    {"www", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE3(3, 3, 3)},
    {"wwwx", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 3, 3, 0)},
    {"wwwy", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 3, 3, 1)},
    {"wwwz", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 3, 3, 2)},
    {"wwww", (getter)Vector_swizzle_get, (setter)NULL, NULL, SWIZZLE4(3, 3, 3, 3)},

#undef AXIS_FROM_CHAR
#undef SWIZZLE1
#undef SWIZZLE2
#undef SWIZZLE3
#undef SWIZZLE4
#undef _SWIZZLE1
#undef _SWIZZLE2
#undef _SWIZZLE3
#undef _SWIZZLE4

    {NULL, NULL, NULL, NULL, NULL} /* Sentinel */
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vector Type: Method Definitions
 * \{ */

static struct PyMethodDef Vector_methods[] = {
    /* Class Methods */
    {"Fill", (PyCFunction)C_Vector_Fill, METH_VARARGS | METH_CLASS, C_Vector_Fill_doc},
    {"Range", (PyCFunction)C_Vector_Range, METH_VARARGS | METH_CLASS, C_Vector_Range_doc},
    {"Linspace", (PyCFunction)C_Vector_Linspace, METH_VARARGS | METH_CLASS, C_Vector_Linspace_doc},
    {"Repeat", (PyCFunction)C_Vector_Repeat, METH_VARARGS | METH_CLASS, C_Vector_Repeat_doc},

    /* In place only. */
    {"zero", (PyCFunction)Vector_zero, METH_NOARGS, Vector_zero_doc},
    {"negate", (PyCFunction)Vector_negate, METH_NOARGS, Vector_negate_doc},

    /* Operate on original or copy. */
    {"normalize", (PyCFunction)Vector_normalize, METH_NOARGS, Vector_normalize_doc},
    {"normalized", (PyCFunction)Vector_normalized, METH_NOARGS, Vector_normalized_doc},

    {"resize", (PyCFunction)Vector_resize, METH_O, Vector_resize_doc},
    {"resized", (PyCFunction)Vector_resized, METH_O, Vector_resized_doc},
    {"to_2d", (PyCFunction)Vector_to_2d, METH_NOARGS, Vector_to_2d_doc},
    {"resize_2d", (PyCFunction)Vector_resize_2d, METH_NOARGS, Vector_resize_2d_doc},
    {"to_3d", (PyCFunction)Vector_to_3d, METH_NOARGS, Vector_to_3d_doc},
    {"resize_3d", (PyCFunction)Vector_resize_3d, METH_NOARGS, Vector_resize_3d_doc},
    {"to_4d", (PyCFunction)Vector_to_4d, METH_NOARGS, Vector_to_4d_doc},
    {"resize_4d", (PyCFunction)Vector_resize_4d, METH_NOARGS, Vector_resize_4d_doc},
    {"to_tuple", (PyCFunction)Vector_to_tuple, METH_VARARGS, Vector_to_tuple_doc},
    {"to_track_quat", (PyCFunction)Vector_to_track_quat, METH_VARARGS, Vector_to_track_quat_doc},
    {"orthogonal", (PyCFunction)Vector_orthogonal, METH_NOARGS, Vector_orthogonal_doc},

    /* Operation between 2 or more types. */
    {"reflect", (PyCFunction)Vector_reflect, METH_O, Vector_reflect_doc},
    {"cross", (PyCFunction)Vector_cross, METH_O, Vector_cross_doc},
    {"dot", (PyCFunction)Vector_dot, METH_O, Vector_dot_doc},
    {"angle", (PyCFunction)Vector_angle, METH_VARARGS, Vector_angle_doc},
    {"angle_signed", (PyCFunction)Vector_angle_signed, METH_VARARGS, Vector_angle_signed_doc},
    {"rotation_difference",
     (PyCFunction)Vector_rotation_difference,
     METH_O,
     Vector_rotation_difference_doc},
    {"project", (PyCFunction)Vector_project, METH_O, Vector_project_doc},
    {"lerp", (PyCFunction)Vector_lerp, METH_VARARGS, Vector_lerp_doc},
    {"slerp", (PyCFunction)Vector_slerp, METH_VARARGS, Vector_slerp_doc},
    {"rotate", (PyCFunction)Vector_rotate, METH_O, Vector_rotate_doc},

    /* Base-math methods. */
    {"freeze", (PyCFunction)BaseMathObject_freeze, METH_NOARGS, BaseMathObject_freeze_doc},

    {"copy", (PyCFunction)Vector_copy, METH_NOARGS, Vector_copy_doc},
    {"__copy__", (PyCFunction)Vector_copy, METH_NOARGS, NULL},
    {"__deepcopy__", (PyCFunction)Vector_deepcopy, METH_VARARGS, NULL},
    {NULL, NULL, 0, NULL},
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vector Type: Python Object Definition
 *
 * \note #Py_TPFLAGS_CHECKTYPES allows us to avoid casting all types to Vector when coercing
 * but this means for eg that (vec * mat) and (mat * vec)
 * both get sent to Vector_mul and it needs to sort out the order
 * \{ */

PyDoc_STRVAR(vector_doc,
             ".. class:: Vector(seq)\n"
             "\n"
             "   This object gives access to Vectors in Blender.\n"
             "\n"
             "   :param seq: Components of the vector, must be a sequence of at least two\n"
             "   :type seq: sequence of numbers\n");
PyTypeObject vector_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    /*  For printing, in format "<module>.<name>" */
    "Vector",             /* char *tp_name; */
    sizeof(VectorObject), /* int tp_basicsize; */
    0,                    /* tp_itemsize;  For allocation */

    /* Methods to implement standard operations */

    (destructor)BaseMathObject_dealloc, /* destructor tp_dealloc; */
    0,                                  /* tp_vectorcall_offset */
    NULL,                               /* getattrfunc tp_getattr; */
    NULL,                               /* setattrfunc tp_setattr; */
    NULL,                               /* cmpfunc tp_compare; */
    (reprfunc)Vector_repr,              /* reprfunc tp_repr; */

    /* Method suites for standard classes */

    &Vector_NumMethods, /* PyNumberMethods *tp_as_number; */
    &Vector_SeqMethods, /* PySequenceMethods *tp_as_sequence; */
    &Vector_AsMapping,  /* PyMappingMethods *tp_as_mapping; */

    /* More standard operations (here for binary compatibility) */

    (hashfunc)Vector_hash, /* hashfunc tp_hash; */
    NULL,                  /* ternaryfunc tp_call; */
#ifndef MATH_STANDALONE
    (reprfunc)Vector_str, /* reprfunc tp_str; */
#else
    NULL, /* reprfunc tp_str; */
#endif
    NULL, /* getattrofunc tp_getattro; */
    NULL, /* setattrofunc tp_setattro; */

    /* Functions to access object as input/output buffer */
    NULL, /* PyBufferProcs *tp_as_buffer; */

    /*** Flags to define presence of optional/expanded features ***/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HAVE_GC,
    vector_doc, /*  char *tp_doc;  Documentation string */
    /*** Assigned meaning in release 2.0 ***/

    /* call function for all accessible objects */
    (traverseproc)BaseMathObject_traverse, /* tp_traverse */

    /* delete references to contained objects */
    (inquiry)BaseMathObject_clear, /* tp_clear */

    /***  Assigned meaning in release 2.1 ***/
    /*** rich comparisons ***/
    (richcmpfunc)Vector_richcmpr, /* richcmpfunc tp_richcompare; */

    /***  weak reference enabler ***/
    0, /* long tp_weaklistoffset; */

    /*** Added in release 2.2 ***/
    /*   Iterators */
    NULL, /* getiterfunc tp_iter; */
    NULL, /* iternextfunc tp_iternext; */

    /*** Attribute descriptor and subclassing stuff ***/
    Vector_methods,   /* struct PyMethodDef *tp_methods; */
    NULL,             /* struct PyMemberDef *tp_members; */
    Vector_getseters, /* struct PyGetSetDef *tp_getset; */
    NULL,             /* struct _typeobject *tp_base; */
    NULL,             /* PyObject *tp_dict; */
    NULL,             /* descrgetfunc tp_descr_get; */
    NULL,             /* descrsetfunc tp_descr_set; */
    0,                /* long tp_dictoffset; */
    NULL,             /* initproc tp_init; */
    NULL,             /* allocfunc tp_alloc; */
    Vector_new,       /* newfunc tp_new; */
    /*  Low-level free-memory routine */
    NULL, /* freefunc tp_free; */
    /* For PyObject_IS_GC */
    NULL, /* inquiry tp_is_gc; */
    NULL, /* PyObject *tp_bases; */
    /* method resolution order */
    NULL, /* PyObject *tp_mro; */
    NULL, /* PyObject *tp_cache; */
    NULL, /* PyObject *tp_subclasses; */
    NULL, /* PyObject *tp_weaklist; */
    NULL,
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vector Type: C/API Constructors
 * \{ */

PyObject *Vector_CreatePyObject(const float *vec, const int vec_num, PyTypeObject *base_type)
{
  VectorObject *self;
  float *vec_alloc;

  if (vec_num < 2) {
    PyErr_SetString(PyExc_RuntimeError, "Vector(): invalid size");
    return NULL;
  }

  vec_alloc = PyMem_Malloc(vec_num * sizeof(float));
  if (UNLIKELY(vec_alloc == NULL)) {
    PyErr_SetString(PyExc_MemoryError,
                    "Vector(): "
                    "problem allocating data");
    return NULL;
  }

  self = BASE_MATH_NEW(VectorObject, vector_Type, base_type);
  if (self) {
    self->vec = vec_alloc;
    self->vec_num = vec_num;

    /* init callbacks as NULL */
    self->cb_user = NULL;
    self->cb_type = self->cb_subtype = 0;

    if (vec) {
      memcpy(self->vec, vec, vec_num * sizeof(float));
    }
    else { /* new empty */
      copy_vn_fl(self->vec, vec_num, 0.0f);
      if (vec_num == 4) { /* do the homogeneous thing */
        self->vec[3] = 1.0f;
      }
    }
    self->flag = BASE_MATH_FLAG_DEFAULT;
  }
  else {
    PyMem_Free(vec_alloc);
  }

  return (PyObject *)self;
}

PyObject *Vector_CreatePyObject_wrap(float *vec, const int vec_num, PyTypeObject *base_type)
{
  VectorObject *self;

  if (vec_num < 2) {
    PyErr_SetString(PyExc_RuntimeError, "Vector(): invalid size");
    return NULL;
  }

  self = BASE_MATH_NEW(VectorObject, vector_Type, base_type);
  if (self) {
    self->vec_num = vec_num;

    /* init callbacks as NULL */
    self->cb_user = NULL;
    self->cb_type = self->cb_subtype = 0;

    self->vec = vec;
    self->flag = BASE_MATH_FLAG_DEFAULT | BASE_MATH_FLAG_IS_WRAP;
  }
  return (PyObject *)self;
}

PyObject *Vector_CreatePyObject_cb(PyObject *cb_user, int vec_num, uchar cb_type, uchar cb_subtype)
{
  VectorObject *self = (VectorObject *)Vector_CreatePyObject(NULL, vec_num, NULL);
  if (self) {
    Py_INCREF(cb_user);
    self->cb_user = cb_user;
    self->cb_type = cb_type;
    self->cb_subtype = cb_subtype;
    PyObject_GC_Track(self);
  }

  return (PyObject *)self;
}

PyObject *Vector_CreatePyObject_alloc(float *vec, const int vec_num, PyTypeObject *base_type)
{
  VectorObject *self;
  self = (VectorObject *)Vector_CreatePyObject_wrap(vec, vec_num, base_type);
  if (self) {
    self->flag &= ~BASE_MATH_FLAG_IS_WRAP;
  }

  return (PyObject *)self;
}

/** \} */
