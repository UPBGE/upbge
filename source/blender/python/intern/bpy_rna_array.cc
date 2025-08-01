/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup pythonintern
 *
 * This file deals with array access for 'BPy_PropertyArrayRNA' from `bpy_rna.cc`.
 */

#include <Python.h>

#include "CLG_log.h"

#include "BLI_utildefines.h"

#include "RNA_types.hh"

#include "bpy_rna.hh"

#include "MEM_guardedalloc.h"

#include "RNA_access.hh"

#include "BPY_extern_clog.hh"

#include "../generic/py_capi_utils.hh"

#define USE_MATHUTILS

#ifdef USE_MATHUTILS
#  include "../mathutils/mathutils.hh" /* so we can have mathutils callbacks */
#endif

#define MAX_ARRAY_DIMENSION 10

struct ItemConvertArgData;

using ItemConvertFunc = void (*)(const ItemConvertArgData *arg, PyObject *py_data, char *data);
using ItemTypeCheckFunc = int (*)(PyObject *py_data);
using RNA_SetArrayFunc = void (*)(PointerRNA *ptr, PropertyRNA *prop, const char *data);
using RNA_SetIndexFunc = void (*)(PointerRNA *ptr, PropertyRNA *prop, int index, void *data_item);

struct ItemConvertArgData {
  union {
    struct {
      int range[2];
    } int_data;
    struct {
      float range[2];
    } float_data;
  };
};

/**
 * Callback and args needed to apply the value (clamp range for now)
 */
struct ItemConvert_FuncArg {
  ItemConvertFunc func;
  ItemConvertArgData arg;
};

/*
 * arr[3][4][5]
 *     0  1  2  <- dimension index
 */

/*
 *  arr[2] = x
 *
 *  py_to_array_index(arraydim=0, arrayoffset=0, index=2)
 *      validate_array(lvalue_dim=0)
 *      ... make real index ...
 */

/* arr[3] = x, self->arraydim is 0, lvalue_dim is 1 */
/* Ensures that a python sequence has expected number of
 * items/sub-items and items are of desired type. */
static int validate_array_type(PyObject *seq,
                               int dim,
                               int totdim,
                               int dimsize[],
                               const bool is_dynamic,
                               ItemTypeCheckFunc check_item_type,
                               const char *item_type_str,
                               const char *error_prefix)
{
  Py_ssize_t i;

  /* not the last dimension */
  if (dim + 1 < totdim) {
    /* check that a sequence contains dimsize[dim] items */
    const int seq_size = PySequence_Size(seq);
    if (seq_size == -1) {
      PyErr_Format(PyExc_ValueError,
                   "%s sequence expected at dimension %d, not '%s'",
                   error_prefix,
                   dim + 1,
                   Py_TYPE(seq)->tp_name);
      return -1;
    }
    for (i = 0; i < seq_size; i++) {
      Py_ssize_t item_seq_size;
      PyObject *item;
      bool ok = true;
      item = PySequence_GetItem(seq, i);

      if (item == nullptr) {
        PyErr_Format(PyExc_TypeError,
                     "%s sequence type '%s' failed to retrieve index %d",
                     error_prefix,
                     Py_TYPE(seq)->tp_name,
                     i);
        ok = false;
      }
      else if ((item_seq_size = PySequence_Size(item)) == -1) {
        PyErr_Format(PyExc_TypeError,
                     "%s expected a sequence of %s, not %s",
                     error_prefix,
                     item_type_str,
                     Py_TYPE(item)->tp_name);
        ok = false;
      }
      /* arr[3][4][5]
       * dimsize[1] = 4
       * dimsize[2] = 5
       *
       * dim = 0 */
      else if (item_seq_size != dimsize[dim + 1]) {
        PyErr_Format(PyExc_ValueError,
                     "%s sequences of dimension %d should contain %d items, not %d",
                     error_prefix,
                     dim + 1,
                     dimsize[dim + 1],
                     item_seq_size);
        ok = false;
      }
      else if (validate_array_type(item,
                                   dim + 1,
                                   totdim,
                                   dimsize,
                                   is_dynamic,
                                   check_item_type,
                                   item_type_str,
                                   error_prefix) == -1)
      {
        ok = false;
      }

      Py_XDECREF(item);

      if (!ok) {
        return -1;
      }
    }
  }
  else {
    /* check that items are of correct type */
    const int seq_size = PySequence_Size(seq);
    if (seq_size == -1) {
      PyErr_Format(PyExc_ValueError,
                   "%s sequence expected at dimension %d, not '%s'",
                   error_prefix,
                   dim + 1,
                   Py_TYPE(seq)->tp_name);
      return -1;
    }
    if ((seq_size != dimsize[dim]) && (is_dynamic == false)) {
      PyErr_Format(PyExc_ValueError,
                   "%s sequences of dimension %d should contain %d items, not %d",
                   error_prefix,
                   dim,
                   dimsize[dim],
                   seq_size);
      return -1;
    }

    for (i = 0; i < seq_size; i++) {
      PyObject *item = PySequence_GetItem(seq, i);

      if (item == nullptr) {
        PyErr_Format(PyExc_TypeError,
                     "%s sequence type '%s' failed to retrieve index %d",
                     error_prefix,
                     Py_TYPE(seq)->tp_name,
                     i);
        return -1;
      }
      if (!check_item_type(item)) {
        Py_DECREF(item);
        PyErr_Format(PyExc_TypeError,
                     "%s expected sequence items of type %s, not %s",
                     error_prefix,
                     item_type_str,
                     Py_TYPE(item)->tp_name);
        return -1;
      }

      Py_DECREF(item);
    }
  }

  return 0; /* ok */
}

/* Returns the number of items in a single- or multi-dimensional sequence. */
static int count_items(PyObject *seq, int dim)
{
  int totitem = 0;

  if (dim > 1) {
    const Py_ssize_t seq_size = PySequence_Size(seq);
    Py_ssize_t i;
    for (i = 0; i < seq_size; i++) {
      PyObject *item = PySequence_GetItem(seq, i);
      if (item) {
        const int tot = count_items(item, dim - 1);
        Py_DECREF(item);
        if (tot != -1) {
          totitem += tot;
        }
        else {
          totitem = -1;
          break;
        }
      }
      else {
        totitem = -1;
        break;
      }
    }
  }
  else {
    totitem = PySequence_Size(seq);
  }

  return totitem;
}

/* Modifies property array length if needed and PROP_DYNAMIC flag is set. */
static int validate_array_length(PyObject *rvalue,
                                 PointerRNA *ptr,
                                 PropertyRNA *prop,
                                 const bool prop_is_param_dyn_alloc,
                                 int lvalue_dim,
                                 int *r_totitem,
                                 const char *error_prefix)
{
  int dimsize[MAX_ARRAY_DIMENSION];
  int tot, totdim, len;

  totdim = RNA_property_array_dimension(ptr, prop, dimsize);
  tot = count_items(rvalue, totdim - lvalue_dim);

  if (tot == -1) {
    PyErr_Format(PyExc_ValueError,
                 "%s %.200s.%.200s, error validating the sequence length",
                 error_prefix,
                 RNA_struct_identifier(ptr->type),
                 RNA_property_identifier(prop));
    return -1;
  }
  if ((RNA_property_flag(prop) & PROP_DYNAMIC) && lvalue_dim == 0) {
    const int tot_expected = RNA_property_array_length(ptr, prop);
    if (tot_expected != tot) {
      *r_totitem = tot;
      if (!prop_is_param_dyn_alloc) {
        PyErr_Format(PyExc_ValueError,
                     "%s %s.%s: array length cannot be changed to %d (expected %d)",
                     error_prefix,
                     RNA_struct_identifier(ptr->type),
                     RNA_property_identifier(prop),
                     tot,
                     tot_expected);
        return -1;
      }
      return 0;
    }

    len = tot;
  }
  else {
    /* length is a constraint */
    if (!lvalue_dim) {
      len = RNA_property_array_length(ptr, prop);
    }
    /* array item assignment */
    else {
      int i;

      len = 1;

      /* arr[3][4][5]
       *
       *    arr[2] = x
       *    dimsize = {4, 5}
       *    dimsize[1] = 4
       *    dimsize[2] = 5
       *    lvalue_dim = 0, totdim = 3
       *
       *    arr[2][3] = x
       *    lvalue_dim = 1
       *
       *    arr[2][3][4] = x
       *    lvalue_dim = 2 */
      for (i = lvalue_dim; i < totdim; i++) {
        len *= dimsize[i];
      }
    }

    if (tot != len) {
      PyErr_Format(PyExc_ValueError,
                   "%s %.200s.%.200s, sequence must have %d items total, not %d",
                   error_prefix,
                   RNA_struct_identifier(ptr->type),
                   RNA_property_identifier(prop),
                   len,
                   tot);
      return -1;
    }
  }

  *r_totitem = len;

  return 0;
}

static int validate_array(PyObject *rvalue,
                          PointerRNA *ptr,
                          PropertyRNA *prop,
                          const bool prop_is_param_dyn_alloc,
                          int lvalue_dim,
                          ItemTypeCheckFunc check_item_type,
                          const char *item_type_str,
                          int *r_totitem,
                          const char *error_prefix)
{
  int dimsize[MAX_ARRAY_DIMENSION];
  const int totdim = RNA_property_array_dimension(ptr, prop, dimsize);

  /* validate type first because length validation may modify property array length */

#ifdef USE_MATHUTILS
  if (lvalue_dim == 0) { /* only valid for first level array */
    if (MatrixObject_Check(rvalue)) {
      MatrixObject *pymat = (MatrixObject *)rvalue;

      if (BaseMath_ReadCallback(pymat) == -1) {
        return -1;
      }

      if (RNA_property_type(prop) != PROP_FLOAT) {
        PyErr_Format(PyExc_ValueError,
                     "%s %.200s.%.200s, matrix assign to non float array",
                     error_prefix,
                     RNA_struct_identifier(ptr->type),
                     RNA_property_identifier(prop));
        return -1;
      }
      if (totdim != 2) {
        PyErr_Format(PyExc_ValueError,
                     "%s %.200s.%.200s, matrix assign array with %d dimensions",
                     error_prefix,
                     RNA_struct_identifier(ptr->type),
                     RNA_property_identifier(prop),
                     totdim);
        return -1;
      }
      if (pymat->col_num != dimsize[0] || pymat->row_num != dimsize[1]) {
        PyErr_Format(PyExc_ValueError,
                     "%s %.200s.%.200s, matrix assign dimension size mismatch, "
                     "is %dx%d, expected be %dx%d",
                     error_prefix,
                     RNA_struct_identifier(ptr->type),
                     RNA_property_identifier(prop),
                     pymat->col_num,
                     pymat->row_num,
                     dimsize[0],
                     dimsize[1]);
        return -1;
      }

      *r_totitem = dimsize[0] * dimsize[1];
      return 0;
    }
  }
#endif /* USE_MATHUTILS */

  {
    const int prop_flag = RNA_property_flag(prop);
    if (validate_array_type(rvalue,
                            lvalue_dim,
                            totdim,
                            dimsize,
                            (prop_flag & PROP_DYNAMIC) != 0,
                            check_item_type,
                            item_type_str,
                            error_prefix) == -1)
    {
      return -1;
    }

    return validate_array_length(
        rvalue, ptr, prop, prop_is_param_dyn_alloc, lvalue_dim, r_totitem, error_prefix);
  }
}

static char *copy_value_single(PyObject *item,
                               PointerRNA *ptr,
                               PropertyRNA *prop,
                               char *data,
                               uint item_size,
                               int *index,
                               const ItemConvert_FuncArg *convert_item,
                               RNA_SetIndexFunc rna_set_index)
{
  if (!data) {
    union {
      float fl;
      int i;
    } value_buf;
    char *value = static_cast<char *>((void *)&value_buf);

    convert_item->func(&convert_item->arg, item, value);
    rna_set_index(ptr, prop, *index, value);
    (*index) += 1;
  }
  else {
    convert_item->func(&convert_item->arg, item, data);
    data += item_size;
  }

  return data;
}

static char *copy_values(PyObject *seq,
                         PointerRNA *ptr,
                         PropertyRNA *prop,
                         int dim,
                         char *data,
                         uint item_size,
                         int *index,
                         const ItemConvert_FuncArg *convert_item,
                         RNA_SetIndexFunc rna_set_index)
{
  const int totdim = RNA_property_array_dimension(ptr, prop, nullptr);
  const Py_ssize_t seq_size = PySequence_Size(seq);
  Py_ssize_t i;

  /* Regarding PySequence_GetItem() failing.
   *
   * This should never be nullptr since we validated it, _but_ some tricky python
   * developer could write their own sequence type which succeeds on
   * validating but fails later somehow, so include checks for safety.
   */

  /* Note that 'data can be nullptr' */

  if (seq_size == -1) {
    return nullptr;
  }

#ifdef USE_MATHUTILS
  if (dim == 0) {
    if (MatrixObject_Check(seq)) {
      MatrixObject *pymat = (MatrixObject *)seq;
      const size_t allocsize = pymat->col_num * pymat->row_num * sizeof(float);

      /* read callback already done by validate */
      /* since this is the first iteration we can assume data is allocated */
      memcpy(data, pymat->matrix, allocsize);

      /* not really needed but do for completeness */
      data += allocsize;

      return data;
    }
  }
#endif /* USE_MATHUTILS */

  for (i = 0; i < seq_size; i++) {
    PyObject *item = PySequence_GetItem(seq, i);
    if (item) {
      if (dim + 1 < totdim) {
        data = copy_values(
            item, ptr, prop, dim + 1, data, item_size, index, convert_item, rna_set_index);
      }
      else {
        data = copy_value_single(
            item, ptr, prop, data, item_size, index, convert_item, rna_set_index);
      }

      Py_DECREF(item);

      /* data may be nullptr, but the for loop checks */
    }
    else {
      return nullptr;
    }
  }

  return data;
}

static int py_to_array(PyObject *seq,
                       PointerRNA *ptr,
                       PropertyRNA *prop,
                       char *param_data,
                       ItemTypeCheckFunc check_item_type,
                       const char *item_type_str,
                       int item_size,
                       const ItemConvert_FuncArg *convert_item,
                       RNA_SetArrayFunc rna_set_array,
                       const char *error_prefix)
{
  // int totdim, dim_size[MAX_ARRAY_DIMENSION];
  int totitem;
  char *data = nullptr;

  // totdim = RNA_property_array_dimension(ptr, prop, dim_size); /* UNUSED */
  const int flag = RNA_property_flag(prop);

  /* Use #ParameterDynAlloc which defines its own array length. */
  const bool prop_is_param_dyn_alloc = param_data && (flag & PROP_DYNAMIC);

  if (validate_array(seq,
                     ptr,
                     prop,
                     prop_is_param_dyn_alloc,
                     0,
                     check_item_type,
                     item_type_str,
                     &totitem,
                     error_prefix) == -1)
  {
    return -1;
  }

  if (totitem) {
    /* NOTE: this code is confusing. */
    if (prop_is_param_dyn_alloc) {
      /* not freeing allocated mem, RNA_parameter_list_free() will do this */
      ParameterDynAlloc *param_alloc = (ParameterDynAlloc *)param_data;
      param_alloc->array_tot = totitem;

      /* freeing param list will free */
      param_alloc->array = MEM_callocN(item_size * totitem, "py_to_array dyn");

      data = static_cast<char *>(param_alloc->array);
    }
    else if (param_data) {
      data = param_data;
    }
    else {
      data = static_cast<char *>(PyMem_MALLOC(item_size * totitem));
    }

    /* will only fail in very rare cases since we already validated the
     * python data, the check here is mainly for completeness. */
    if (copy_values(seq, ptr, prop, 0, data, item_size, nullptr, convert_item, nullptr) != nullptr)
    {
      if (param_data == nullptr) {
        /* nullptr can only pass through in case RNA property array-length is 0 (impossible?) */
        rna_set_array(ptr, prop, data);
        PyMem_FREE(data);
      }
    }
    else {
      if (param_data == nullptr) {
        PyMem_FREE(data);
      }

      PyErr_Format(PyExc_TypeError,
                   "%s internal error parsing sequence of type '%s' after successful validation",
                   error_prefix,
                   Py_TYPE(seq)->tp_name);
      return -1;
    }
  }

  return 0;
}

static int py_to_array_index(PyObject *py,
                             PointerRNA *ptr,
                             PropertyRNA *prop,
                             int lvalue_dim,
                             int arrayoffset,
                             int index,
                             ItemTypeCheckFunc check_item_type,
                             const char *item_type_str,
                             const ItemConvert_FuncArg *convert_item,
                             RNA_SetIndexFunc rna_set_index,
                             const char *error_prefix)
{
  int totdim, dimsize[MAX_ARRAY_DIMENSION];
  int totitem, i;

  totdim = RNA_property_array_dimension(ptr, prop, dimsize);

  /* convert index */

  /* arr[3][4][5]
   *
   *    arr[2] = x
   *    lvalue_dim = 0, index = 0 + 2 * 4 * 5
   *
   *    arr[2][3] = x
   *    lvalue_dim = 1, index = 40 + 3 * 5 */

  lvalue_dim++;

  for (i = lvalue_dim; i < totdim; i++) {
    index *= dimsize[i];
  }

  index += arrayoffset;

  if (lvalue_dim == totdim) { /* single item, assign directly */
    if (!check_item_type(py)) {
      PyErr_Format(PyExc_TypeError,
                   "%s %.200s.%.200s, expected a %s type, not %s",
                   error_prefix,
                   RNA_struct_identifier(ptr->type),
                   RNA_property_identifier(prop),
                   item_type_str,
                   Py_TYPE(py)->tp_name);
      return -1;
    }
    copy_value_single(py, ptr, prop, nullptr, 0, &index, convert_item, rna_set_index);
  }
  else {
    const bool prop_is_param_dyn_alloc = false;
    if (validate_array(py,
                       ptr,
                       prop,
                       prop_is_param_dyn_alloc,
                       lvalue_dim,
                       check_item_type,
                       item_type_str,
                       &totitem,
                       error_prefix) == -1)
    {
      return -1;
    }

    if (totitem) {
      copy_values(py, ptr, prop, lvalue_dim, nullptr, 0, &index, convert_item, rna_set_index);
    }
  }
  return 0;
}

static void py_to_float(const ItemConvertArgData *arg, PyObject *py, char *data)
{
  const float *range = arg->float_data.range;
  float value = float(PyFloat_AsDouble(py));
  CLAMP(value, range[0], range[1]);
  *(float *)data = value;
}

static void py_to_int(const ItemConvertArgData *arg, PyObject *py, char *data)
{
  const int *range = arg->int_data.range;
  int value = PyC_Long_AsI32(py);
  CLAMP(value, range[0], range[1]);
  *(int *)data = value;
}

static void py_to_bool(const ItemConvertArgData * /*arg*/, PyObject *py, char *data)
{
  *(bool *)data = bool(PyObject_IsTrue(py));
}

static int py_float_check(PyObject *py)
{
  /* accept both floats and integers */
  return PyNumber_Check(py);
}

static int py_int_check(PyObject *py)
{
  /* accept only integers */
  return PyLong_Check(py);
}

static int py_bool_check(PyObject *py)
{
  return PyBool_Check(py);
}

static void float_set_index(PointerRNA *ptr, PropertyRNA *prop, int index, void *value)
{
  RNA_property_float_set_index(ptr, prop, index, *(float *)value);
}

static void int_set_index(PointerRNA *ptr, PropertyRNA *prop, int index, void *value)
{
  RNA_property_int_set_index(ptr, prop, index, *(int *)value);
}

static void bool_set_index(PointerRNA *ptr, PropertyRNA *prop, int index, void *value)
{
  RNA_property_boolean_set_index(ptr, prop, index, *(bool *)value);
}

static void convert_item_init_float(PointerRNA *ptr,
                                    PropertyRNA *prop,
                                    ItemConvert_FuncArg *convert_item)
{
  float *range = convert_item->arg.float_data.range;
  convert_item->func = py_to_float;
  RNA_property_float_range(ptr, prop, &range[0], &range[1]);
}

static void convert_item_init_int(PointerRNA *ptr,
                                  PropertyRNA *prop,
                                  ItemConvert_FuncArg *convert_item)
{
  int *range = convert_item->arg.int_data.range;
  convert_item->func = py_to_int;
  RNA_property_int_range(ptr, prop, &range[0], &range[1]);
}

static void convert_item_init_bool(PointerRNA * /*ptr*/,
                                   PropertyRNA * /*prop*/,
                                   ItemConvert_FuncArg *convert_item)
{
  convert_item->func = py_to_bool;
}

int pyrna_py_to_array(
    PointerRNA *ptr, PropertyRNA *prop, char *param_data, PyObject *py, const char *error_prefix)
{
  int ret;
  switch (RNA_property_type(prop)) {
    case PROP_FLOAT: {
      ItemConvert_FuncArg convert_item;
      convert_item_init_float(ptr, prop, &convert_item);

      ret = py_to_array(py,
                        ptr,
                        prop,
                        param_data,
                        py_float_check,
                        "float",
                        sizeof(float),
                        &convert_item,
                        (RNA_SetArrayFunc)RNA_property_float_set_array,
                        error_prefix);
      break;
    }
    case PROP_INT: {
      ItemConvert_FuncArg convert_item;
      convert_item_init_int(ptr, prop, &convert_item);

      ret = py_to_array(py,
                        ptr,
                        prop,
                        param_data,
                        py_int_check,
                        "int",
                        sizeof(int),
                        &convert_item,
                        (RNA_SetArrayFunc)RNA_property_int_set_array,
                        error_prefix);
      break;
    }
    case PROP_BOOLEAN: {
      ItemConvert_FuncArg convert_item;
      convert_item_init_bool(ptr, prop, &convert_item);

      ret = py_to_array(py,
                        ptr,
                        prop,
                        param_data,
                        py_bool_check,
                        "boolean",
                        sizeof(bool),
                        &convert_item,
                        (RNA_SetArrayFunc)RNA_property_boolean_set_array,
                        error_prefix);
      break;
    }
    default: {
      PyErr_SetString(PyExc_TypeError, "not an array type");
      ret = -1;
      break;
    }
  }

  return ret;
}

int pyrna_py_to_array_index(PointerRNA *ptr,
                            PropertyRNA *prop,
                            int arraydim,
                            int arrayoffset,
                            int index,
                            PyObject *py,
                            const char *error_prefix)
{
  int ret;
  switch (RNA_property_type(prop)) {
    case PROP_FLOAT: {
      ItemConvert_FuncArg convert_item;
      convert_item_init_float(ptr, prop, &convert_item);

      ret = py_to_array_index(py,
                              ptr,
                              prop,
                              arraydim,
                              arrayoffset,
                              index,
                              py_float_check,
                              "float",
                              &convert_item,
                              float_set_index,
                              error_prefix);
      break;
    }
    case PROP_INT: {
      ItemConvert_FuncArg convert_item;
      convert_item_init_int(ptr, prop, &convert_item);

      ret = py_to_array_index(py,
                              ptr,
                              prop,
                              arraydim,
                              arrayoffset,
                              index,
                              py_int_check,
                              "int",
                              &convert_item,
                              int_set_index,
                              error_prefix);
      break;
    }
    case PROP_BOOLEAN: {
      ItemConvert_FuncArg convert_item;
      convert_item_init_bool(ptr, prop, &convert_item);

      ret = py_to_array_index(py,
                              ptr,
                              prop,
                              arraydim,
                              arrayoffset,
                              index,
                              py_bool_check,
                              "boolean",
                              &convert_item,
                              bool_set_index,
                              error_prefix);
      break;
    }
    default: {
      PyErr_SetString(PyExc_TypeError, "not an array type");
      ret = -1;
      break;
    }
  }

  return ret;
}

PyObject *pyrna_array_index(PointerRNA *ptr, PropertyRNA *prop, int index)
{
  PyObject *item;

  switch (RNA_property_type(prop)) {
    case PROP_FLOAT:
      item = PyFloat_FromDouble(RNA_property_float_get_index(ptr, prop, index));
      break;
    case PROP_BOOLEAN:
      item = PyBool_FromLong(RNA_property_boolean_get_index(ptr, prop, index));
      break;
    case PROP_INT:
      item = PyLong_FromLong(RNA_property_int_get_index(ptr, prop, index));
      break;
    default:
      PyErr_SetString(PyExc_TypeError, "not an array type");
      item = nullptr;
      break;
  }

  return item;
}

#if 0
/* XXX this is not used (and never will?) */
/* Given an array property, creates an N-dimensional tuple of values. */
static PyObject *pyrna_py_from_array_internal(PointerRNA *ptr,
                                              PropertyRNA *prop,
                                              int dim,
                                              int *index)
{
  PyObject *tuple;
  int i, len;
  int totdim = RNA_property_array_dimension(ptr, prop, nullptr);

  len = RNA_property_multi_array_length(ptr, prop, dim);

  tuple = PyTuple_New(len);

  for (i = 0; i < len; i++) {
    PyObject *item;

    if (dim + 1 < totdim) {
      item = pyrna_py_from_array_internal(ptr, prop, dim + 1, index);
    }
    else {
      item = pyrna_array_index(ptr, prop, *index);
      *index = *index + 1;
    }

    if (!item) {
      Py_DECREF(tuple);
      return nullptr;
    }

    PyTuple_SET_ITEM(tuple, i, item);
  }

  return tuple;
}
#endif

PyObject *pyrna_py_from_array_index(BPy_PropertyArrayRNA *self,
                                    PointerRNA *ptr,
                                    PropertyRNA *prop,
                                    int index)
{
  int totdim, arraydim, arrayoffset, dimsize[MAX_ARRAY_DIMENSION], i, len;
  BPy_PropertyArrayRNA *ret = nullptr;

  arraydim = self ? self->arraydim : 0;
  arrayoffset = self ? self->arrayoffset : 0;

  /* just in case check */
  len = RNA_property_multi_array_length(ptr, prop, arraydim);
  if (index >= len || index < 0) {
    /* This shouldn't happen because higher level functions must check for invalid index. */
    CLOG_WARN(BPY_LOG_RNA, "invalid index %d for array with length=%d", index, len);

    PyErr_SetString(PyExc_IndexError, "out of range");
    return nullptr;
  }

  totdim = RNA_property_array_dimension(ptr, prop, dimsize);

  if (arraydim + 1 < totdim) {
    ret = (BPy_PropertyArrayRNA *)pyrna_prop_CreatePyObject(ptr, prop);
    ret->arraydim = arraydim + 1;

    /* arr[3][4][5]
     *
     *    x = arr[2]
     *    index = 0 + 2 * 4 * 5
     *
     *    x = arr[2][3]
     *    index = offset + 3 * 5 */

    for (i = arraydim + 1; i < totdim; i++) {
      index *= dimsize[i];
    }

    ret->arrayoffset = arrayoffset + index;
  }
  else {
    index = arrayoffset + index;
    ret = (BPy_PropertyArrayRNA *)pyrna_array_index(ptr, prop, index);
  }

  return (PyObject *)ret;
}

PyObject *pyrna_py_from_array(PointerRNA *ptr, PropertyRNA *prop)
{
  PyObject *ret;

  ret = pyrna_math_object_from_array(ptr, prop);

  /* Is this a math object? */
  if (ret) {
    return ret;
  }

  return pyrna_prop_CreatePyObject(ptr, prop);
}

int pyrna_array_contains_py(PointerRNA *ptr, PropertyRNA *prop, PyObject *value)
{
  /* TODO: multi-dimensional arrays. */

  const int len = RNA_property_array_length(ptr, prop);
  int type;
  int i;

  if (len == 0) {
    /* possible with dynamic arrays */
    return 0;
  }

  if (RNA_property_array_dimension(ptr, prop, nullptr) > 1) {
    PyErr_SetString(PyExc_TypeError, "PropertyRNA - multi dimensional arrays not supported yet");
    return -1;
  }

  type = RNA_property_type(prop);

  switch (type) {
    case PROP_FLOAT: {
      const float value_f = PyFloat_AsDouble(value);
      if (value_f == -1 && PyErr_Occurred()) {
        PyErr_Clear();
        return 0;
      }

      float tmp[32];
      float *tmp_arr;

      if (len * sizeof(float) > sizeof(tmp)) {
        tmp_arr = static_cast<float *>(PyMem_MALLOC(len * sizeof(float)));
      }
      else {
        tmp_arr = tmp;
      }

      RNA_property_float_get_array(ptr, prop, tmp_arr);

      for (i = 0; i < len; i++) {
        if (tmp_arr[i] == value_f) {
          break;
        }
      }

      if (tmp_arr != tmp) {
        PyMem_FREE(tmp_arr);
      }

      return i < len ? 1 : 0;
    }
    case PROP_INT: {
      const int value_i = PyC_Long_AsI32(value);
      if (value_i == -1 && PyErr_Occurred()) {
        PyErr_Clear();
        return 0;
      }

      int tmp[32];
      int *tmp_arr;

      if (len * sizeof(int) > sizeof(tmp)) {
        tmp_arr = static_cast<int *>(PyMem_MALLOC(len * sizeof(int)));
      }
      else {
        tmp_arr = tmp;
      }

      RNA_property_int_get_array(ptr, prop, tmp_arr);

      for (i = 0; i < len; i++) {
        if (tmp_arr[i] == value_i) {
          break;
        }
      }

      if (tmp_arr != tmp) {
        PyMem_FREE(tmp_arr);
      }

      return i < len ? 1 : 0;
    }
    case PROP_BOOLEAN: {
      const int value_i = PyC_Long_AsBool(value);
      if (value_i == -1 && PyErr_Occurred()) {
        PyErr_Clear();
        return 0;
      }

      bool tmp[32];
      bool *tmp_arr;

      if (len * sizeof(bool) > sizeof(tmp)) {
        tmp_arr = static_cast<bool *>(PyMem_MALLOC(len * sizeof(bool)));
      }
      else {
        tmp_arr = tmp;
      }

      RNA_property_boolean_get_array(ptr, prop, tmp_arr);

      for (i = 0; i < len; i++) {
        if (tmp_arr[i] == bool(value_i)) {
          break;
        }
      }

      if (tmp_arr != tmp) {
        PyMem_FREE(tmp_arr);
      }

      return i < len ? 1 : 0;
    }
  }

  /* should never reach this */
  PyErr_SetString(PyExc_TypeError, "PropertyRNA - type not in float/bool/int");
  return -1;
}
