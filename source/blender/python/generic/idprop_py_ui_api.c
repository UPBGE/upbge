/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup pygen
 */

#include <Python.h>

#include "MEM_guardedalloc.h"

#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "idprop_py_ui_api.h"

#include "BKE_idprop.h"

#include "DNA_ID.h"

#include "RNA_access.h"
#include "RNA_enum_types.h"

#define USE_STRING_COERCE

#ifdef USE_STRING_COERCE
#  include "py_capi_utils.h"
#endif
#include "py_capi_rna.h"

#include "python_utildefines.h"

/* -------------------------------------------------------------------- */
/** \name UI Data Update
 * \{ */

static bool args_contain_key(PyObject *kwargs, const char *name)
{
  if (kwargs == NULL) {
    /* When a function gets called without any kwargs, Python just passes NULL instead.
     * PyDict_Contains() is not NULL-safe, though. */
    return false;
  }

  PyObject *py_key = PyUnicode_FromString(name);
  const bool result = PyDict_Contains(kwargs, py_key) == 1;
  Py_DECREF(py_key);
  return result;
}

/**
 * \return False when parsing fails, in which case caller should return NULL.
 */
static bool idprop_ui_data_update_base(IDPropertyUIData *ui_data,
                                       const char *rna_subtype,
                                       const char *description)
{
  if (rna_subtype != NULL) {
    if (pyrna_enum_value_from_id(rna_enum_property_subtype_items,
                                 rna_subtype,
                                 &ui_data->rna_subtype,
                                 "IDPropertyUIManager.update") == -1) {
      return false;
    }
  }

  if (description != NULL) {
    ui_data->description = BLI_strdup(description);
  }

  return true;
}

/**
 * \note The default value needs special handling because for array IDProperties it can
 * be a single value or an array, but for non-array properties it can only be a value.
 */
static bool idprop_ui_data_update_int_default(IDProperty *idprop,
                                              IDPropertyUIDataInt *ui_data,
                                              PyObject *default_value)
{
  if (PySequence_Check(default_value)) {
    if (idprop->type != IDP_ARRAY) {
      PyErr_SetString(PyExc_TypeError, "Only array properties can have array default values");
      return false;
    }

    Py_ssize_t len = PySequence_Size(default_value);
    int *new_default_array = (int *)MEM_malloc_arrayN(len, sizeof(int), __func__);
    if (PyC_AsArray(
            new_default_array, sizeof(int), default_value, len, &PyLong_Type, "ui_data_update") ==
        -1) {
      MEM_freeN(new_default_array);
      return false;
    }

    ui_data->default_array_len = len;
    ui_data->default_array = new_default_array;
  }
  else {
    const int value = PyC_Long_AsI32(default_value);
    if ((value == -1) && PyErr_Occurred()) {
      PyErr_SetString(PyExc_ValueError, "Error converting \"default\" argument to integer");
      return false;
    }
    ui_data->default_value = value;
  }

  return true;
}

/**
 * \return False when parsing fails, in which case caller should return NULL.
 */
static bool idprop_ui_data_update_int(IDProperty *idprop, PyObject *args, PyObject *kwargs)
{
  const char *rna_subtype = NULL;
  const char *description = NULL;
  int min, max, soft_min, soft_max, step;
  PyObject *default_value = NULL;
  const char *kwlist[] = {
      "min", "max", "soft_min", "soft_max", "step", "default", "subtype", "description", NULL};
  if (!PyArg_ParseTupleAndKeywords(args,
                                   kwargs,
                                   "|$iiiiiOzz:update",
                                   (char **)kwlist,
                                   &min,
                                   &max,
                                   &soft_min,
                                   &soft_max,
                                   &step,
                                   &default_value,
                                   &rna_subtype,
                                   &description)) {
    return false;
  }

  /* Write to a temporary copy of the UI data in case some part of the parsing fails. */
  IDPropertyUIDataInt *ui_data_orig = (IDPropertyUIDataInt *)idprop->ui_data;
  IDPropertyUIDataInt ui_data = *ui_data_orig;

  if (!idprop_ui_data_update_base(&ui_data.base, rna_subtype, description)) {
    IDP_ui_data_free_unique_contents(&ui_data.base, IDP_ui_data_type(idprop), &ui_data_orig->base);
    return false;
  }

  if (args_contain_key(kwargs, "min")) {
    ui_data.min = min;
    ui_data.soft_min = MAX2(ui_data.soft_min, ui_data.min);
    ui_data.max = MAX2(ui_data.min, ui_data.max);
  }
  if (args_contain_key(kwargs, "max")) {
    ui_data.max = max;
    ui_data.soft_max = MIN2(ui_data.soft_max, ui_data.max);
    ui_data.min = MIN2(ui_data.min, ui_data.max);
  }
  if (args_contain_key(kwargs, "soft_min")) {
    ui_data.soft_min = soft_min;
    ui_data.soft_min = MAX2(ui_data.soft_min, ui_data.min);
    ui_data.soft_max = MAX2(ui_data.soft_min, ui_data.soft_max);
  }
  if (args_contain_key(kwargs, "soft_max")) {
    ui_data.soft_max = soft_max;
    ui_data.soft_max = MIN2(ui_data.soft_max, ui_data.max);
    ui_data.soft_min = MIN2(ui_data.soft_min, ui_data.soft_max);
  }
  if (args_contain_key(kwargs, "step")) {
    ui_data.step = step;
  }

  if (!ELEM(default_value, NULL, Py_None)) {
    if (!idprop_ui_data_update_int_default(idprop, &ui_data, default_value)) {
      IDP_ui_data_free_unique_contents(
          &ui_data.base, IDP_ui_data_type(idprop), &ui_data_orig->base);
      return false;
    }
  }

  /* Write back to the property's UI data. */
  IDP_ui_data_free_unique_contents(&ui_data_orig->base, IDP_ui_data_type(idprop), &ui_data.base);
  *ui_data_orig = ui_data;
  return true;
}

/**
 * \note The default value needs special handling because for array IDProperties it can
 * be a single value or an array, but for non-array properties it can only be a value.
 */
static bool idprop_ui_data_update_float_default(IDProperty *idprop,
                                                IDPropertyUIDataFloat *ui_data,
                                                PyObject *default_value)
{
  if (PySequence_Check(default_value)) {
    if (idprop->type != IDP_ARRAY) {
      PyErr_SetString(PyExc_TypeError, "Only array properties can have array default values");
      return false;
    }

    Py_ssize_t len = PySequence_Size(default_value);
    double *new_default_array = (double *)MEM_malloc_arrayN(len, sizeof(double), __func__);
    if (PyC_AsArray(new_default_array,
                    sizeof(double),
                    default_value,
                    len,
                    &PyFloat_Type,
                    "ui_data_update") == -1) {
      MEM_freeN(new_default_array);
      return false;
    }

    ui_data->default_array_len = len;
    ui_data->default_array = new_default_array;
  }
  else {
    const double value = PyFloat_AsDouble(default_value);
    if ((value == -1.0) && PyErr_Occurred()) {
      PyErr_SetString(PyExc_ValueError, "Error converting \"default\" argument to double");
      return false;
    }
    ui_data->default_value = value;
  }

  return true;
}

/**
 * \return False when parsing fails, in which case caller should return NULL.
 */
static bool idprop_ui_data_update_float(IDProperty *idprop, PyObject *args, PyObject *kwargs)
{
  const char *rna_subtype = NULL;
  const char *description = NULL;
  int precision;
  double min, max, soft_min, soft_max, step;
  PyObject *default_value = NULL;
  const char *kwlist[] = {"min",
                          "max",
                          "soft_min",
                          "soft_max",
                          "step",
                          "precision",
                          "default",
                          "subtype",
                          "description",
                          NULL};
  if (!PyArg_ParseTupleAndKeywords(args,
                                   kwargs,
                                   "|$dddddiOzz:update",
                                   (char **)kwlist,
                                   &min,
                                   &max,
                                   &soft_min,
                                   &soft_max,
                                   &step,
                                   &precision,
                                   &default_value,
                                   &rna_subtype,
                                   &description)) {
    return false;
  }

  /* Write to a temporary copy of the UI data in case some part of the parsing fails. */
  IDPropertyUIDataFloat *ui_data_orig = (IDPropertyUIDataFloat *)idprop->ui_data;
  IDPropertyUIDataFloat ui_data = *ui_data_orig;

  if (!idprop_ui_data_update_base(&ui_data.base, rna_subtype, description)) {
    IDP_ui_data_free_unique_contents(&ui_data.base, IDP_ui_data_type(idprop), &ui_data_orig->base);
    return false;
  }

  if (args_contain_key(kwargs, "min")) {
    ui_data.min = min;
    ui_data.soft_min = MAX2(ui_data.soft_min, ui_data.min);
    ui_data.max = MAX2(ui_data.min, ui_data.max);
  }
  if (args_contain_key(kwargs, "max")) {
    ui_data.max = max;
    ui_data.soft_max = MIN2(ui_data.soft_max, ui_data.max);
    ui_data.min = MIN2(ui_data.min, ui_data.max);
  }
  if (args_contain_key(kwargs, "soft_min")) {
    ui_data.soft_min = soft_min;
    ui_data.soft_min = MAX2(ui_data.soft_min, ui_data.min);
    ui_data.soft_max = MAX2(ui_data.soft_min, ui_data.soft_max);
  }
  if (args_contain_key(kwargs, "soft_max")) {
    ui_data.soft_max = soft_max;
    ui_data.soft_max = MIN2(ui_data.soft_max, ui_data.max);
    ui_data.soft_min = MIN2(ui_data.soft_min, ui_data.soft_max);
  }
  if (args_contain_key(kwargs, "step")) {
    ui_data.step = (float)step;
  }
  if (args_contain_key(kwargs, "precision")) {
    ui_data.precision = precision;
  }

  if (!ELEM(default_value, NULL, Py_None)) {
    if (!idprop_ui_data_update_float_default(idprop, &ui_data, default_value)) {
      IDP_ui_data_free_unique_contents(
          &ui_data.base, IDP_ui_data_type(idprop), &ui_data_orig->base);
      return false;
    }
  }

  /* Write back to the property's UI data. */
  IDP_ui_data_free_unique_contents(&ui_data_orig->base, IDP_ui_data_type(idprop), &ui_data.base);
  *ui_data_orig = ui_data;
  return true;
}

/**
 * \return False when parsing fails, in which case caller should return NULL.
 */
static bool idprop_ui_data_update_string(IDProperty *idprop, PyObject *args, PyObject *kwargs)
{
  const char *rna_subtype = NULL;
  const char *description = NULL;
  const char *default_value;
  const char *kwlist[] = {"default", "subtype", "description", NULL};
  if (!PyArg_ParseTupleAndKeywords(args,
                                   kwargs,
                                   "|$zzz:update",
                                   (char **)kwlist,
                                   &default_value,
                                   &rna_subtype,
                                   &description)) {
    return false;
  }

  /* Write to a temporary copy of the UI data in case some part of the parsing fails. */
  IDPropertyUIDataString *ui_data_orig = (IDPropertyUIDataString *)idprop->ui_data;
  IDPropertyUIDataString ui_data = *ui_data_orig;

  if (!idprop_ui_data_update_base(&ui_data.base, rna_subtype, description)) {
    IDP_ui_data_free_unique_contents(&ui_data.base, IDP_ui_data_type(idprop), &ui_data_orig->base);
    return false;
  }

  if (default_value != NULL) {
    ui_data.default_value = BLI_strdup(default_value);
  }

  /* Write back to the property's UI data. */
  IDP_ui_data_free_unique_contents(&ui_data_orig->base, IDP_ui_data_type(idprop), &ui_data.base);
  *ui_data_orig = ui_data;
  return true;
}

/**
 * \return False when parsing fails, in which case caller should return NULL.
 */
static bool idprop_ui_data_update_id(IDProperty *idprop, PyObject *args, PyObject *kwargs)
{
  const char *rna_subtype = NULL;
  const char *description = NULL;
  const char *kwlist[] = {"subtype", "description", NULL};
  if (!PyArg_ParseTupleAndKeywords(
          args, kwargs, "|$zz:update", (char **)kwlist, &rna_subtype, &description)) {
    return false;
  }

  /* Write to a temporary copy of the UI data in case some part of the parsing fails. */
  IDPropertyUIDataID *ui_data_orig = (IDPropertyUIDataID *)idprop->ui_data;
  IDPropertyUIDataID ui_data = *ui_data_orig;

  if (!idprop_ui_data_update_base(&ui_data.base, rna_subtype, description)) {
    IDP_ui_data_free_unique_contents(&ui_data.base, IDP_ui_data_type(idprop), &ui_data_orig->base);
    return false;
  }

  /* Write back to the property's UI data. */
  IDP_ui_data_free_unique_contents(&ui_data_orig->base, IDP_ui_data_type(idprop), &ui_data.base);
  *ui_data_orig = ui_data;
  return true;
}

PyDoc_STRVAR(BPy_IDPropertyUIManager_update_doc,
             ".. method:: update( "
             "subtype=None, "
             "min=None, "
             "max=None, "
             "soft_min=None, "
             "soft_max=None, "
             "precision=None, "
             "step=None, "
             "default=None, "
             "description=None)\n"
             "\n"
             "   Update the RNA information of the IDProperty used for interaction and\n"
             "   display in the user interface. The required types for many of the keyword\n"
             "   arguments depend on the type of the property.\n ");
static PyObject *BPy_IDPropertyUIManager_update(BPy_IDPropertyUIManager *self,
                                                PyObject *args,
                                                PyObject *kwargs)
{
  IDProperty *property = self->property;
  BLI_assert(IDP_ui_data_supported(property));

  switch (IDP_ui_data_type(property)) {
    case IDP_UI_DATA_TYPE_INT:
      IDP_ui_data_ensure(property);
      if (!idprop_ui_data_update_int(property, args, kwargs)) {
        return NULL;
      }
      Py_RETURN_NONE;
    case IDP_UI_DATA_TYPE_FLOAT:
      IDP_ui_data_ensure(property);
      if (!idprop_ui_data_update_float(property, args, kwargs)) {
        return NULL;
      }
      Py_RETURN_NONE;
    case IDP_UI_DATA_TYPE_STRING:
      IDP_ui_data_ensure(property);
      if (!idprop_ui_data_update_string(property, args, kwargs)) {
        return NULL;
      }
      Py_RETURN_NONE;
    case IDP_UI_DATA_TYPE_ID:
      IDP_ui_data_ensure(property);
      if (!idprop_ui_data_update_id(property, args, kwargs)) {
        return NULL;
      }
      Py_RETURN_NONE;
    case IDP_UI_DATA_TYPE_UNSUPPORTED:
      PyErr_Format(PyExc_TypeError, "IDProperty \"%s\" does not support RNA data", property->name);
      return NULL;
  }

  BLI_assert_unreachable();
  Py_RETURN_NONE;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name UI Data As Dictionary
 * \{ */

static void idprop_ui_data_to_dict_int(IDProperty *property, PyObject *dict)
{
  IDPropertyUIDataInt *ui_data = (IDPropertyUIDataInt *)property->ui_data;
  PyObject *item;

  PyDict_SetItemString(dict, "min", item = PyLong_FromLong(ui_data->min));
  Py_DECREF(item);
  PyDict_SetItemString(dict, "max", item = PyLong_FromLong(ui_data->max));
  Py_DECREF(item);
  PyDict_SetItemString(dict, "soft_min", item = PyLong_FromLong(ui_data->soft_min));
  Py_DECREF(item);
  PyDict_SetItemString(dict, "soft_max", item = PyLong_FromLong(ui_data->soft_max));
  Py_DECREF(item);
  PyDict_SetItemString(dict, "step", item = PyLong_FromLong(ui_data->step));
  Py_DECREF(item);
  if (property->type == IDP_ARRAY) {
    PyObject *list = PyList_New(ui_data->default_array_len);
    for (int i = 0; i < ui_data->default_array_len; i++) {
      PyList_SET_ITEM(list, i, PyLong_FromLong(ui_data->default_array[i]));
    }
    PyDict_SetItemString(dict, "default", list);
    Py_DECREF(list);
  }
  else {
    PyDict_SetItemString(dict, "default", item = PyLong_FromLong(ui_data->default_value));
    Py_DECREF(item);
  }
}

static void idprop_ui_data_to_dict_float(IDProperty *property, PyObject *dict)
{
  IDPropertyUIDataFloat *ui_data = (IDPropertyUIDataFloat *)property->ui_data;
  PyObject *item;

  PyDict_SetItemString(dict, "min", item = PyFloat_FromDouble(ui_data->min));
  Py_DECREF(item);
  PyDict_SetItemString(dict, "max", item = PyFloat_FromDouble(ui_data->max));
  Py_DECREF(item);
  PyDict_SetItemString(dict, "soft_min", item = PyFloat_FromDouble(ui_data->soft_min));
  Py_DECREF(item);
  PyDict_SetItemString(dict, "soft_max", item = PyFloat_FromDouble(ui_data->soft_max));
  Py_DECREF(item);
  PyDict_SetItemString(dict, "step", item = PyFloat_FromDouble((double)ui_data->step));
  Py_DECREF(item);
  PyDict_SetItemString(dict, "precision", item = PyLong_FromDouble((double)ui_data->precision));
  Py_DECREF(item);
  if (property->type == IDP_ARRAY) {
    PyObject *list = PyList_New(ui_data->default_array_len);
    for (int i = 0; i < ui_data->default_array_len; i++) {
      PyList_SET_ITEM(list, i, PyFloat_FromDouble(ui_data->default_array[i]));
    }
    PyDict_SetItemString(dict, "default", list);
    Py_DECREF(list);
  }
  else {
    PyDict_SetItemString(dict, "default", item = PyFloat_FromDouble(ui_data->default_value));
    Py_DECREF(item);
  }
}

static void idprop_ui_data_to_dict_string(IDProperty *property, PyObject *dict)
{
  IDPropertyUIDataString *ui_data = (IDPropertyUIDataString *)property->ui_data;
  PyObject *item;

  const char *default_value = (ui_data->default_value == NULL) ? "" : ui_data->default_value;

  PyDict_SetItemString(dict, "default", item = PyUnicode_FromString(default_value));
  Py_DECREF(item);
}

PyDoc_STRVAR(BPy_IDPropertyUIManager_as_dict_doc,
             ".. method:: as_dict()\n"
             "\n"
             "   Return a dictionary of the property's RNA UI data. The fields in the\n"
             "   returned dictionary and their types will depend on the property's type.\n");
static PyObject *BPy_IDIDPropertyUIManager_as_dict(BPy_IDPropertyUIManager *self)
{
  IDProperty *property = self->property;
  BLI_assert(IDP_ui_data_supported(property));

  IDPropertyUIData *ui_data = IDP_ui_data_ensure(property);

  PyObject *dict = PyDict_New();

  /* RNA subtype. */
  {
    const char *subtype_id = NULL;
    RNA_enum_identifier(rna_enum_property_subtype_items, ui_data->rna_subtype, &subtype_id);
    PyObject *item = PyUnicode_FromString(subtype_id);
    PyDict_SetItemString(dict, "subtype", item);
    Py_DECREF(item);
  }

  /* Description. */
  if (ui_data->description != NULL) {
    PyObject *item = PyUnicode_FromString(ui_data->description);
    PyDict_SetItemString(dict, "description", item);
    Py_DECREF(item);
  }

  /* Type specific data. */
  switch (IDP_ui_data_type(property)) {
    case IDP_UI_DATA_TYPE_STRING:
      idprop_ui_data_to_dict_string(property, dict);
      break;
    case IDP_UI_DATA_TYPE_ID:
      break;
    case IDP_UI_DATA_TYPE_INT:
      idprop_ui_data_to_dict_int(property, dict);
      break;
    case IDP_UI_DATA_TYPE_FLOAT:
      idprop_ui_data_to_dict_float(property, dict);
      break;
    case IDP_UI_DATA_TYPE_UNSUPPORTED:
      BLI_assert_unreachable();
      break;
  }

  return dict;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name UI Data Clear
 * \{ */

PyDoc_STRVAR(BPy_IDPropertyUIManager_clear_doc,
             ".. method:: clear()\n"
             "\n"
             "   Remove the RNA UI data from this IDProperty.\n");
static PyObject *BPy_IDPropertyUIManager_clear(BPy_IDPropertyUIManager *self)
{
  IDProperty *property = self->property;
  BLI_assert(IDP_ui_data_supported(property));

  if (property == NULL) {
    PyErr_SetString(PyExc_RuntimeError, "IDPropertyUIManager missing property");
    BLI_assert_unreachable();
    return NULL;
  }

  if (property->ui_data != NULL) {
    IDP_ui_data_free(property);
  }

  Py_RETURN_NONE;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name UI Data Copying
 * \{ */

PyDoc_STRVAR(
    BPy_IDPropertyUIManager_update_from_doc,
    ".. method:: update_from(ui_manager_source)\n"
    "\n"
    "   Copy UI data from an IDProperty in the source group to a property in this group.\n "
    "   If the source property has no UI data, the target UI data will be reset if it exists.\n"
    "\n"
    "   :raises TypeError: If the types of the two properties don't match.\n");
static PyObject *BPy_IDPropertyUIManager_update_from(BPy_IDPropertyUIManager *self, PyObject *args)
{
  IDProperty *property = self->property;
  BLI_assert(IDP_ui_data_supported(property));

  BPy_IDPropertyUIManager *ui_manager_src;
  if (!PyArg_ParseTuple(args, "O!:update_from", &BPy_IDPropertyUIManager_Type, &ui_manager_src)) {
    return NULL;
  }

  if (property->ui_data != NULL) {
    IDP_ui_data_free(property);
  }

  if (ui_manager_src->property && ui_manager_src->property->ui_data) {
    property->ui_data = IDP_ui_data_copy(ui_manager_src->property);
  }

  Py_RETURN_NONE;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name UI Data Manager Definition
 * \{ */

static struct PyMethodDef BPy_IDPropertyUIManager_methods[] = {
    {"update",
     (PyCFunction)BPy_IDPropertyUIManager_update,
     METH_VARARGS | METH_KEYWORDS,
     BPy_IDPropertyUIManager_update_doc},
    {"as_dict",
     (PyCFunction)BPy_IDIDPropertyUIManager_as_dict,
     METH_NOARGS,
     BPy_IDPropertyUIManager_as_dict_doc},
    {"clear",
     (PyCFunction)BPy_IDPropertyUIManager_clear,
     METH_NOARGS,
     BPy_IDPropertyUIManager_clear_doc},
    {"update_from",
     (PyCFunction)BPy_IDPropertyUIManager_update_from,
     METH_VARARGS,
     BPy_IDPropertyUIManager_update_from_doc},
    {NULL, NULL, 0, NULL},
};

static PyObject *BPy_IDPropertyUIManager_repr(BPy_IDPropertyUIManager *self)
{
  return PyUnicode_FromFormat(
      "<bpy id prop ui manager: name=\"%s\", address=%p>", self->property->name, self->property);
}

static Py_hash_t BPy_IDPropertyUIManager_hash(BPy_IDPropertyUIManager *self)
{
  return _Py_HashPointer(self->property);
}

PyTypeObject BPy_IDPropertyUIManager_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    /*  For printing, in format "<module>.<name>" */
    "IDPropertyUIManager",           /* char *tp_name; */
    sizeof(BPy_IDPropertyUIManager), /* int tp_basicsize; */
    0,                               /* tp_itemsize;  For allocation */

    /* Methods to implement standard operations */

    NULL,                                   /* destructor tp_dealloc; */
    0,                                      /* tp_vectorcall_offset */
    NULL,                                   /* getattrfunc tp_getattr; */
    NULL,                                   /* setattrfunc tp_setattr; */
    NULL,                                   /* cmpfunc tp_compare; */
    (reprfunc)BPy_IDPropertyUIManager_repr, /* reprfunc tp_repr; */

    /* Method suites for standard classes */

    NULL, /* PyNumberMethods *tp_as_number; */
    NULL, /* PySequenceMethods *tp_as_sequence; */
    NULL, /* PyMappingMethods *tp_as_mapping; */

    /* More standard operations (here for binary compatibility) */

    (hashfunc)BPy_IDPropertyUIManager_hash, /* hashfunc tp_hash; */
    NULL,                                   /* ternaryfunc tp_call; */
    NULL,                                   /* reprfunc tp_str; */
    NULL,                                   /* getattrofunc tp_getattro; */
    NULL,                                   /* setattrofunc tp_setattro; */

    /* Functions to access object as input/output buffer */
    NULL, /* PyBufferProcs *tp_as_buffer; */

    /*** Flags to define presence of optional/expanded features ***/
    Py_TPFLAGS_DEFAULT, /* long tp_flags; */

    NULL, /*  char *tp_doc;  Documentation string */
    /*** Assigned meaning in release 2.0 ***/
    /* call function for all accessible objects */
    NULL, /* traverseproc tp_traverse; */

    /* delete references to contained objects */
    NULL, /* inquiry tp_clear; */

    /***  Assigned meaning in release 2.1 ***/
    /*** rich comparisons ***/
    NULL, /* richcmpfunc tp_richcompare; */

    /***  weak reference enabler ***/
    0, /* long tp_weaklistoffset; */

    /*** Added in release 2.2 ***/
    /*   Iterators */
    NULL, /* getiterfunc tp_iter; */
    NULL, /* iternextfunc tp_iternext; */
    /*** Attribute descriptor and subclassing stuff ***/
    BPy_IDPropertyUIManager_methods, /* struct PyMethodDef *tp_methods; */
    NULL,                            /* struct PyMemberDef *tp_members; */
    NULL,                            /* struct PyGetSetDef *tp_getset; */
};

void IDPropertyUIData_Init_Types()
{
  PyType_Ready(&BPy_IDPropertyUIManager_Type);
}

/** \} */
