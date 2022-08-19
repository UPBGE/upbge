/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup pygen
 */

#include <Python.h>

#include "MEM_guardedalloc.h"

#include "BLI_utildefines.h"

#include "idprop_py_api.h"
#include "idprop_py_ui_api.h"

#include "BKE_idprop.h"

#include "DNA_ID.h" /* ID property definitions. */

#define USE_STRING_COERCE

#ifdef USE_STRING_COERCE
#  include "py_capi_utils.h"
#endif

#include "python_utildefines.h"

extern bool pyrna_id_FromPyObject(PyObject *obj, ID **id);
extern PyObject *pyrna_id_CreatePyObject(ID *id);
extern bool pyrna_id_CheckPyObject(PyObject *obj);

/* Currently there is no need to expose this publicly. */
static PyObject *BPy_IDGroup_IterKeys_CreatePyObject(BPy_IDProperty *group, const bool reversed);
static PyObject *BPy_IDGroup_IterValues_CreatePyObject(BPy_IDProperty *group, const bool reversed);
static PyObject *BPy_IDGroup_IterItems_CreatePyObject(BPy_IDProperty *group, const bool reversed);

static PyObject *BPy_IDGroup_ViewKeys_CreatePyObject(BPy_IDProperty *group);
static PyObject *BPy_IDGroup_ViewValues_CreatePyObject(BPy_IDProperty *group);
static PyObject *BPy_IDGroup_ViewItems_CreatePyObject(BPy_IDProperty *group);

static BPy_IDGroup_View *IDGroup_View_New_WithType(BPy_IDProperty *group, PyTypeObject *type);
static int BPy_IDGroup_Contains(BPy_IDProperty *self, PyObject *value);

/* -------------------------------------------------------------------- */
/** \name Python from ID-Property (Internal Conversions)
 *
 * Low level conversion to avoid duplicate code, no type checking.
 * \{ */

static PyObject *idprop_py_from_idp_string(const IDProperty *prop)
{
  if (prop->subtype == IDP_STRING_SUB_BYTE) {
    return PyBytes_FromStringAndSize(IDP_String(prop), prop->len);
  }

#ifdef USE_STRING_COERCE
  return PyC_UnicodeFromByteAndSize(IDP_Array(prop), prop->len - 1);
#else
  return PyUnicode_FromStringAndSize(IDP_String(prop), prop->len - 1);
#endif
}

static PyObject *idprop_py_from_idp_int(const IDProperty *prop)
{
  return PyLong_FromLong((long)IDP_Int(prop));
}

static PyObject *idprop_py_from_idp_float(const IDProperty *prop)
{
  return PyFloat_FromDouble((double)IDP_Float(prop));
}

static PyObject *idprop_py_from_idp_double(const IDProperty *prop)
{
  return PyFloat_FromDouble(IDP_Double(prop));
}

static PyObject *idprop_py_from_idp_group(ID *id, IDProperty *prop, IDProperty *parent)
{
  BPy_IDProperty *group = PyObject_New(BPy_IDProperty, &BPy_IDGroup_Type);
  group->owner_id = id;
  group->prop = prop;
  group->parent = parent; /* can be NULL */
  return (PyObject *)group;
}

static PyObject *idprop_py_from_idp_id(IDProperty *prop)
{
  return pyrna_id_CreatePyObject(prop->data.pointer);
}

static PyObject *idprop_py_from_idp_array(ID *id, IDProperty *prop)
{
  BPy_IDProperty *array = PyObject_New(BPy_IDProperty, &BPy_IDArray_Type);
  array->owner_id = id;
  array->prop = prop;
  return (PyObject *)array;
}

static PyObject *idprop_py_from_idp_idparray(ID *id, IDProperty *prop)
{
  PyObject *seq = PyList_New(prop->len);
  IDProperty *array = IDP_IDPArray(prop);
  int i;

  if (!seq) {
    PyErr_Format(
        PyExc_RuntimeError, "%s: IDP_IDPARRAY: PyList_New(%d) failed", __func__, prop->len);
    return NULL;
  }

  for (i = 0; i < prop->len; i++) {
    PyObject *wrap = BPy_IDGroup_WrapData(id, array++, prop);

    /* BPy_IDGroup_MapDataToPy sets the error */
    if (UNLIKELY(wrap == NULL)) {
      Py_DECREF(seq);
      return NULL;
    }

    PyList_SET_ITEM(seq, i, wrap);
  }

  return seq;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name IDProp Group Access
 * \{ */

/* use for both array and group */
static Py_hash_t BPy_IDGroup_hash(BPy_IDProperty *self)
{
  return _Py_HashPointer(self->prop);
}

static PyObject *BPy_IDGroup_repr(BPy_IDProperty *self)
{
  return PyUnicode_FromFormat("<bpy id prop: owner=\"%s\", name=\"%s\", address=%p>",
                              self->owner_id ? self->owner_id->name : "<NONE>",
                              self->prop->name,
                              self->prop);
}

PyObject *BPy_IDGroup_WrapData(ID *id, IDProperty *prop, IDProperty *parent)
{
  switch (prop->type) {
    case IDP_STRING:
      return idprop_py_from_idp_string(prop);
    case IDP_INT:
      return idprop_py_from_idp_int(prop);
    case IDP_FLOAT:
      return idprop_py_from_idp_float(prop);
    case IDP_DOUBLE:
      return idprop_py_from_idp_double(prop);
    case IDP_GROUP:
      return idprop_py_from_idp_group(id, prop, parent);
    case IDP_ARRAY:
      return idprop_py_from_idp_array(id, prop);
    case IDP_IDPARRAY:
      return idprop_py_from_idp_idparray(id, prop); /* this could be better a internal type */
    case IDP_ID:
      return idprop_py_from_idp_id(prop);
    default:
      Py_RETURN_NONE;
  }
}

/* UNUSED, currently assignment overwrites into new properties, rather than setting in-place. */
#if 0
static int BPy_IDGroup_SetData(BPy_IDProperty *self, IDProperty *prop, PyObject *value)
{
  switch (prop->type) {
    case IDP_STRING: {
      char *st;
      if (!PyUnicode_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "expected a string!");
        return -1;
      }
      /* NOTE: if this code is enabled, bytes support needs to be added */
#  ifdef USE_STRING_COERCE
      {
        int alloc_len;
        PyObject *value_coerce = NULL;

        st = (char *)PyC_UnicodeAsByte(value, &value_coerce);
        alloc_len = strlen(st) + 1;

        st = PyUnicode_AsUTF8(value);
        IDP_ResizeArray(prop, alloc_len);
        memcpy(IDP_Array(prop), st, alloc_len);
        Py_XDECREF(value_coerce);
      }
#  else
      st = PyUnicode_AsUTF8(value);
      IDP_ResizeArray(prop, strlen(st) + 1);
      strcpy(IDP_Array(prop), st);
#  endif

      return 0;
    }

    case IDP_INT: {
      int ivalue = PyLong_AsSsize_t(value);
      if (ivalue == -1 && PyErr_Occurred()) {
        PyErr_SetString(PyExc_TypeError, "expected an int type");
        return -1;
      }
      IDP_Int(prop) = ivalue;
      break;
    }
    case IDP_FLOAT: {
      float fvalue = (float)PyFloat_AsDouble(value);
      if (fvalue == -1 && PyErr_Occurred()) {
        PyErr_SetString(PyExc_TypeError, "expected a float");
        return -1;
      }
      IDP_Float(self->prop) = fvalue;
      break;
    }
    case IDP_DOUBLE: {
      double dvalue = PyFloat_AsDouble(value);
      if (dvalue == -1 && PyErr_Occurred()) {
        PyErr_SetString(PyExc_TypeError, "expected a float");
        return -1;
      }
      IDP_Double(self->prop) = dvalue;
      break;
    }
    default:
      PyErr_SetString(PyExc_AttributeError, "attempt to set read-only attribute!");
      return -1;
  }
  return 0;
}
#endif

static PyObject *BPy_IDGroup_GetName(BPy_IDProperty *self, void *UNUSED(closure))
{
  return PyUnicode_FromString(self->prop->name);
}

static int BPy_IDGroup_SetName(BPy_IDProperty *self, PyObject *value, void *UNUSED(closure))
{
  const char *name;
  Py_ssize_t name_size;

  if (!PyUnicode_Check(value)) {
    PyErr_SetString(PyExc_TypeError, "expected a string!");
    return -1;
  }

  name = PyUnicode_AsUTF8AndSize(value, &name_size);

  if (name_size >= MAX_IDPROP_NAME) {
    PyErr_SetString(PyExc_TypeError, "string length cannot exceed 63 characters!");
    return -1;
  }

  memcpy(self->prop->name, name, name_size);
  return 0;
}

#if 0
static PyObject *BPy_IDGroup_GetType(BPy_IDProperty *self)
{
  return PyLong_FromLong(self->prop->type);
}
#endif

static PyGetSetDef BPy_IDGroup_getseters[] = {
    {"name",
     (getter)BPy_IDGroup_GetName,
     (setter)BPy_IDGroup_SetName,
     "The name of this Group.",
     NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static Py_ssize_t BPy_IDGroup_Map_Len(BPy_IDProperty *self)
{
  if (self->prop->type != IDP_GROUP) {
    PyErr_SetString(PyExc_TypeError, "len() of unsized object");
    return -1;
  }

  return self->prop->len;
}

static PyObject *BPy_IDGroup_Map_GetItem(BPy_IDProperty *self, PyObject *item)
{
  IDProperty *idprop;
  const char *name;

  if (self->prop->type != IDP_GROUP) {
    PyErr_SetString(PyExc_TypeError, "unsubscriptable object");
    return NULL;
  }

  name = PyUnicode_AsUTF8(item);

  if (name == NULL) {
    PyErr_SetString(PyExc_TypeError, "only strings are allowed as keys of ID properties");
    return NULL;
  }

  idprop = IDP_GetPropertyFromGroup(self->prop, name);

  if (idprop == NULL) {
    PyErr_SetString(PyExc_KeyError, "key not in subgroup dict");
    return NULL;
  }

  return BPy_IDGroup_WrapData(self->owner_id, idprop, self->prop);
}

/* returns NULL on success, error string on failure */
static char idp_sequence_type(PyObject *seq_fast)
{
  PyObject **seq_fast_items = PySequence_Fast_ITEMS(seq_fast);
  PyObject *item;
  char type = IDP_INT;

  Py_ssize_t i, len = PySequence_Fast_GET_SIZE(seq_fast);

  for (i = 0; i < len; i++) {
    item = seq_fast_items[i];
    if (PyFloat_Check(item)) {
      if (type == IDP_IDPARRAY) { /* mixed dict/int */
        return -1;
      }
      type = IDP_DOUBLE;
    }
    else if (PyLong_Check(item)) {
      if (type == IDP_IDPARRAY) { /* mixed dict/int */
        return -1;
      }
    }
    else if (PyMapping_Check(item)) {
      if (i != 0 && (type != IDP_IDPARRAY)) { /* mixed dict/int */
        return -1;
      }
      type = IDP_IDPARRAY;
    }
    else {
      return -1;
    }
  }

  return type;
}

static const char *idp_try_read_name(PyObject *name_obj)
{
  const char *name = NULL;
  if (name_obj) {
    Py_ssize_t name_size;
    name = PyUnicode_AsUTF8AndSize(name_obj, &name_size);

    if (name == NULL) {
      PyErr_Format(PyExc_KeyError,
                   "invalid id-property key, expected a string, not a %.200s",
                   Py_TYPE(name_obj)->tp_name);
      return NULL;
    }

    if (name_size >= MAX_IDPROP_NAME) {
      PyErr_SetString(PyExc_KeyError,
                      "the length of IDProperty names is limited to 63 characters");
      return NULL;
    }
  }
  else {
    name = "";
  }
  return name;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name ID-Property from Python (Internal Conversions)
 * \{ */

/**
 * The 'idp_from_Py*' functions expect that the input type has been checked before
 * and return NULL if the IDProperty can't be created.
 */

static IDProperty *idp_from_PyFloat(const char *name, PyObject *ob)
{
  IDPropertyTemplate val = {0};
  val.d = PyFloat_AsDouble(ob);
  return IDP_New(IDP_DOUBLE, &val, name);
}

static IDProperty *idp_from_PyLong(const char *name, PyObject *ob)
{
  IDPropertyTemplate val = {0};
  val.i = PyC_Long_AsI32(ob);
  if (val.i == -1 && PyErr_Occurred()) {
    return NULL;
  }
  return IDP_New(IDP_INT, &val, name);
}

static IDProperty *idp_from_PyUnicode(const char *name, PyObject *ob)
{
  IDProperty *prop;
  IDPropertyTemplate val = {0};
#ifdef USE_STRING_COERCE
  Py_ssize_t value_size;
  PyObject *value_coerce = NULL;
  val.string.str = PyC_UnicodeAsByteAndSize(ob, &value_size, &value_coerce);
  val.string.len = (int)value_size + 1;
  val.string.subtype = IDP_STRING_SUB_UTF8;
  prop = IDP_New(IDP_STRING, &val, name);
  Py_XDECREF(value_coerce);
#else
  val.str = PyUnicode_AsUTF8(ob);
  prop = IDP_New(IDP_STRING, val, name);
#endif
  return prop;
}

static IDProperty *idp_from_PyBytes(const char *name, PyObject *ob)
{
  IDPropertyTemplate val = {0};
  val.string.str = PyBytes_AS_STRING(ob);
  val.string.len = PyBytes_GET_SIZE(ob);
  val.string.subtype = IDP_STRING_SUB_BYTE;
  return IDP_New(IDP_STRING, &val, name);
}

static int idp_array_type_from_formatstr_and_size(const char *typestr, Py_ssize_t itemsize)
{
  const char format = PyC_StructFmt_type_from_str(typestr);

  if (PyC_StructFmt_type_is_float_any(format)) {
    if (itemsize == 4) {
      return IDP_FLOAT;
    }
    if (itemsize == 8) {
      return IDP_DOUBLE;
    }
  }
  if (PyC_StructFmt_type_is_int_any(format)) {
    if (itemsize == 4) {
      return IDP_INT;
    }
  }

  return -1;
}

static const char *idp_format_from_array_type(int type)
{
  if (type == IDP_INT) {
    return "i";
  }
  if (type == IDP_FLOAT) {
    return "f";
  }
  if (type == IDP_DOUBLE) {
    return "d";
  }
  return NULL;
}

static IDProperty *idp_from_PySequence_Buffer(const char *name, Py_buffer *buffer)
{
  IDProperty *prop;
  IDPropertyTemplate val = {0};

  const int id_type = idp_array_type_from_formatstr_and_size(buffer->format, buffer->itemsize);
  if (id_type == -1) {
    /* should never happen as the type has been checked before */
    return NULL;
  }

  val.array.type = id_type;
  val.array.len = buffer->len / buffer->itemsize;

  prop = IDP_New(IDP_ARRAY, &val, name);
  memcpy(IDP_Array(prop), buffer->buf, buffer->len);
  return prop;
}

static IDProperty *idp_from_PySequence_Fast(const char *name, PyObject *ob)
{
  IDProperty *prop;
  IDPropertyTemplate val = {0};

  PyObject **ob_seq_fast_items;
  PyObject *item;
  int i;

  ob_seq_fast_items = PySequence_Fast_ITEMS(ob);

  if ((val.array.type = idp_sequence_type(ob)) == (char)-1) {
    PyErr_SetString(PyExc_TypeError,
                    "only floats, ints and dicts are allowed in ID property arrays");
    return NULL;
  }

  /* validate sequence and derive type.
   * we assume IDP_INT unless we hit a float
   * number; then we assume it's */

  val.array.len = PySequence_Fast_GET_SIZE(ob);

  switch (val.array.type) {
    case IDP_DOUBLE: {
      double *prop_data;
      prop = IDP_New(IDP_ARRAY, &val, name);
      prop_data = IDP_Array(prop);
      for (i = 0; i < val.array.len; i++) {
        item = ob_seq_fast_items[i];
        if (((prop_data[i] = PyFloat_AsDouble(item)) == -1.0) && PyErr_Occurred()) {
          IDP_FreeProperty(prop);
          return NULL;
        }
      }
      break;
    }
    case IDP_INT: {
      int *prop_data;
      prop = IDP_New(IDP_ARRAY, &val, name);
      prop_data = IDP_Array(prop);
      for (i = 0; i < val.array.len; i++) {
        item = ob_seq_fast_items[i];
        if (((prop_data[i] = PyC_Long_AsI32(item)) == -1) && PyErr_Occurred()) {
          IDP_FreeProperty(prop);
          return NULL;
        }
      }
      break;
    }
    case IDP_IDPARRAY: {
      prop = IDP_NewIDPArray(name);
      for (i = 0; i < val.array.len; i++) {
        item = ob_seq_fast_items[i];
        if (BPy_IDProperty_Map_ValidateAndCreate(NULL, prop, item) == false) {
          IDP_FreeProperty(prop);
          return NULL;
        }
      }
      break;
    }
    default:
      /* should never happen */
      PyErr_SetString(PyExc_RuntimeError, "internal error with idp array.type");
      return NULL;
  }
  return prop;
}

static IDProperty *idp_from_PySequence(const char *name, PyObject *ob)
{
  Py_buffer buffer;
  bool use_buffer = false;

  if (PyObject_CheckBuffer(ob)) {
    PyObject_GetBuffer(ob, &buffer, PyBUF_SIMPLE | PyBUF_FORMAT);
    const char format = PyC_StructFmt_type_from_str(buffer.format);
    if (PyC_StructFmt_type_is_float_any(format) ||
        (PyC_StructFmt_type_is_int_any(format) && buffer.itemsize == 4)) {
      use_buffer = true;
    }
    else {
      PyBuffer_Release(&buffer);
    }
  }

  if (use_buffer) {
    IDProperty *prop = idp_from_PySequence_Buffer(name, &buffer);
    PyBuffer_Release(&buffer);
    return prop;
  }

  PyObject *ob_seq_fast = PySequence_Fast(ob, "py -> idprop");
  if (ob_seq_fast != NULL) {
    IDProperty *prop = idp_from_PySequence_Fast(name, ob_seq_fast);
    Py_DECREF(ob_seq_fast);
    return prop;
  }

  return NULL;
}

static IDProperty *idp_from_PyMapping(const char *name, PyObject *ob)
{
  IDProperty *prop;
  const IDPropertyTemplate val = {0};

  PyObject *keys, *vals, *key, *pval;
  int i, len;
  /* yay! we get into recursive stuff now! */
  keys = PyMapping_Keys(ob);
  vals = PyMapping_Values(ob);

  /* We allocate the group first; if we hit any invalid data,
   * we can delete it easily enough. */
  prop = IDP_New(IDP_GROUP, &val, name);
  len = PyMapping_Length(ob);
  for (i = 0; i < len; i++) {
    key = PySequence_GetItem(keys, i);
    pval = PySequence_GetItem(vals, i);
    if (BPy_IDProperty_Map_ValidateAndCreate(key, prop, pval) == false) {
      IDP_FreeProperty(prop);
      Py_XDECREF(keys);
      Py_XDECREF(vals);
      Py_XDECREF(key);
      Py_XDECREF(pval);
      /* error is already set */
      return NULL;
    }
    Py_XDECREF(key);
    Py_XDECREF(pval);
  }
  Py_XDECREF(keys);
  Py_XDECREF(vals);
  return prop;
}

static IDProperty *idp_from_DatablockPointer(const char *name, PyObject *ob)
{
  IDPropertyTemplate val = {0};
  pyrna_id_FromPyObject(ob, &val.id);
  return IDP_New(IDP_ID, &val, name);
}

static IDProperty *idp_from_PyObject(PyObject *name_obj, PyObject *ob)
{
  const char *name = idp_try_read_name(name_obj);
  if (name == NULL) {
    return NULL;
  }

  if (PyFloat_Check(ob)) {
    return idp_from_PyFloat(name, ob);
  }
  if (PyLong_Check(ob)) {
    return idp_from_PyLong(name, ob);
  }
  if (PyUnicode_Check(ob)) {
    return idp_from_PyUnicode(name, ob);
  }
  if (PyBytes_Check(ob)) {
    return idp_from_PyBytes(name, ob);
  }
  if (PySequence_Check(ob)) {
    return idp_from_PySequence(name, ob);
  }
  if (ob == Py_None || pyrna_id_CheckPyObject(ob)) {
    return idp_from_DatablockPointer(name, ob);
  }
  if (PyMapping_Check(ob)) {
    return idp_from_PyMapping(name, ob);
  }

  PyErr_Format(
      PyExc_TypeError, "invalid id-property type %.200s not supported", Py_TYPE(ob)->tp_name);
  return NULL;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Mapping Get/Set (Internal Access)
 * \{ */

bool BPy_IDProperty_Map_ValidateAndCreate(PyObject *name_obj, IDProperty *group, PyObject *ob)
{
  IDProperty *prop = idp_from_PyObject(name_obj, ob);
  if (prop == NULL) {
    return false;
  }

  if (group->type == IDP_IDPARRAY) {
    IDP_AppendArray(group, prop);
    /* IDP_AppendArray does a shallow copy (memcpy), only free memory */
    MEM_freeN(prop);
  }
  else {
    IDProperty *prop_exist;

    /* avoid freeing when types match in case they are referenced by the UI, see: T37073
     * obviously this isn't a complete solution, but helps for common cases. */
    prop_exist = IDP_GetPropertyFromGroup(group, prop->name);
    if ((prop_exist != NULL) && (prop_exist->type == prop->type) &&
        (prop_exist->subtype == prop->subtype)) {
      /* Preserve prev/next links!!! See T42593. */
      prop->prev = prop_exist->prev;
      prop->next = prop_exist->next;
      prop->flag = prop_exist->flag;

      /* Don't free and reset the existing property's UI data, since this only assigns a value. */
      IDPropertyUIData *ui_data = prop_exist->ui_data;
      prop_exist->ui_data = NULL;
      IDP_FreePropertyContent(prop_exist);
      *prop_exist = *prop;
      prop_exist->ui_data = ui_data;
      MEM_freeN(prop);
    }
    else {
      IDP_ReplaceInGroup_ex(group, prop, prop_exist);
    }
  }

  return true;
}

int BPy_Wrap_SetMapItem(IDProperty *prop, PyObject *key, PyObject *val)
{
  if (prop->type != IDP_GROUP) {
    PyErr_SetString(PyExc_TypeError, "unsubscriptable object");
    return -1;
  }

  if (val == NULL) { /* del idprop[key] */
    IDProperty *pkey;
    const char *name = PyUnicode_AsUTF8(key);

    if (name == NULL) {
      PyErr_Format(PyExc_KeyError, "expected a string, not %.200s", Py_TYPE(key)->tp_name);
      return -1;
    }

    pkey = IDP_GetPropertyFromGroup(prop, name);
    if (pkey) {
      IDP_FreeFromGroup(prop, pkey);
      return 0;
    }

    PyErr_SetString(PyExc_KeyError, "property not found in group");
    return -1;
  }

  bool ok;

  ok = BPy_IDProperty_Map_ValidateAndCreate(key, prop, val);
  if (ok == false) {
    return -1;
  }

  return 0;
}

static int BPy_IDGroup_Map_SetItem(BPy_IDProperty *self, PyObject *key, PyObject *val)
{
  return BPy_Wrap_SetMapItem(self->prop, key, val);
}

static PyObject *BPy_IDGroup_iter(BPy_IDProperty *self)
{
  PyObject *iterable = BPy_IDGroup_ViewKeys_CreatePyObject(self);
  PyObject *ret;
  if (iterable) {
    ret = PyObject_GetIter(iterable);
    Py_DECREF(iterable);
  }
  else {
    ret = NULL;
  }
  return ret;
}

PyObject *BPy_IDGroup_MapDataToPy(IDProperty *prop)
{
  switch (prop->type) {
    case IDP_STRING:
      return idprop_py_from_idp_string(prop);
    case IDP_INT:
      return idprop_py_from_idp_int(prop);
    case IDP_FLOAT:
      return idprop_py_from_idp_float(prop);
    case IDP_DOUBLE:
      return idprop_py_from_idp_double(prop);
    case IDP_ID:
      return idprop_py_from_idp_id(prop);
    case IDP_ARRAY: {
      PyObject *seq = PyList_New(prop->len);
      int i;

      if (!seq) {
        PyErr_Format(
            PyExc_RuntimeError, "%s: IDP_ARRAY: PyList_New(%d) failed", __func__, prop->len);
        return NULL;
      }

      switch (prop->subtype) {
        case IDP_FLOAT: {
          const float *array = (float *)IDP_Array(prop);
          for (i = 0; i < prop->len; i++) {
            PyList_SET_ITEM(seq, i, PyFloat_FromDouble(array[i]));
          }
          break;
        }
        case IDP_DOUBLE: {
          const double *array = (double *)IDP_Array(prop);
          for (i = 0; i < prop->len; i++) {
            PyList_SET_ITEM(seq, i, PyFloat_FromDouble(array[i]));
          }
          break;
        }
        case IDP_INT: {
          const int *array = (int *)IDP_Array(prop);
          for (i = 0; i < prop->len; i++) {
            PyList_SET_ITEM(seq, i, PyLong_FromLong(array[i]));
          }
          break;
        }
        default:
          PyErr_Format(
              PyExc_RuntimeError, "%s: invalid/corrupt array type '%d'!", __func__, prop->subtype);
          Py_DECREF(seq);
          return NULL;
      }

      return seq;
    }
    case IDP_IDPARRAY: {
      PyObject *seq = PyList_New(prop->len);
      IDProperty *array = IDP_IDPArray(prop);
      int i;

      if (!seq) {
        PyErr_Format(
            PyExc_RuntimeError, "%s: IDP_IDPARRAY: PyList_New(%d) failed", __func__, prop->len);
        return NULL;
      }

      for (i = 0; i < prop->len; i++) {
        PyObject *wrap = BPy_IDGroup_MapDataToPy(array++);

        /* BPy_IDGroup_MapDataToPy sets the error */
        if (UNLIKELY(wrap == NULL)) {
          Py_DECREF(seq);
          return NULL;
        }

        PyList_SET_ITEM(seq, i, wrap);
      }
      return seq;
    }
    case IDP_GROUP: {
      PyObject *dict = _PyDict_NewPresized(prop->len);
      IDProperty *loop;

      for (loop = prop->data.group.first; loop; loop = loop->next) {
        PyObject *wrap = BPy_IDGroup_MapDataToPy(loop);

        /* BPy_IDGroup_MapDataToPy sets the error */
        if (UNLIKELY(wrap == NULL)) {
          Py_DECREF(dict);
          return NULL;
        }

        PyDict_SetItemString(dict, loop->name, wrap);
        Py_DECREF(wrap);
      }
      return dict;
    }
  }

  PyErr_Format(PyExc_RuntimeError,
               "%s ERROR: '%s' property exists with a bad type code '%d'!",
               __func__,
               prop->name,
               prop->type);
  return NULL;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name ID-Property Group Iterator Type
 * \{ */

static PyObject *BPy_IDGroup_Iter_repr(BPy_IDGroup_Iter *self)
{
  if (self->group == NULL) {
    return PyUnicode_FromFormat("<%s>", Py_TYPE(self)->tp_name);
  }
  return PyUnicode_FromFormat("<%s \"%s\">", Py_TYPE(self)->tp_name, self->group->prop->name);
}

static void BPy_IDGroup_Iter_dealloc(BPy_IDGroup_Iter *self)
{
  if (self->group != NULL) {
    PyObject_GC_UnTrack(self);
  }
  Py_CLEAR(self->group);
  PyObject_GC_Del(self);
}

static int BPy_IDGroup_Iter_traverse(BPy_IDGroup_Iter *self, visitproc visit, void *arg)
{
  Py_VISIT(self->group);
  return 0;
}

static int BPy_IDGroup_Iter_clear(BPy_IDGroup_Iter *self)
{
  Py_CLEAR(self->group);
  return 0;
}

static bool BPy_Group_Iter_same_size_or_raise_error(BPy_IDGroup_Iter *self)
{
  if (self->len_init == self->group->prop->len) {
    return true;
  }
  PyErr_SetString(PyExc_RuntimeError, "IDPropertyGroup changed size during iteration");
  return false;
}

static PyObject *BPy_Group_IterKeys_next(BPy_IDGroup_Iter *self)
{
  if (self->cur != NULL) {
    /* When `cur` is set, `group` cannot be NULL. */
    if (!BPy_Group_Iter_same_size_or_raise_error(self)) {
      return NULL;
    }
    IDProperty *cur = self->cur;
    self->cur = self->reversed ? self->cur->prev : self->cur->next;
    return PyUnicode_FromString(cur->name);
  }
  PyErr_SetNone(PyExc_StopIteration);
  return NULL;
}

static PyObject *BPy_Group_IterValues_next(BPy_IDGroup_Iter *self)
{
  if (self->cur != NULL) {
    /* When `cur` is set, `group` cannot be NULL. */
    if (!BPy_Group_Iter_same_size_or_raise_error(self)) {
      return NULL;
    }
    IDProperty *cur = self->cur;
    self->cur = self->reversed ? self->cur->prev : self->cur->next;
    return BPy_IDGroup_WrapData(self->group->owner_id, cur, self->group->prop);
  }
  PyErr_SetNone(PyExc_StopIteration);
  return NULL;
}

static PyObject *BPy_Group_IterItems_next(BPy_IDGroup_Iter *self)
{
  if (self->cur != NULL) {
    /* When `cur` is set, `group` cannot be NULL. */
    if (!BPy_Group_Iter_same_size_or_raise_error(self)) {
      return NULL;
    }
    IDProperty *cur = self->cur;
    self->cur = self->reversed ? self->cur->prev : self->cur->next;
    PyObject *ret = PyTuple_New(2);
    PyTuple_SET_ITEMS(ret,
                      PyUnicode_FromString(cur->name),
                      BPy_IDGroup_WrapData(self->group->owner_id, cur, self->group->prop));
    return ret;
  }
  PyErr_SetNone(PyExc_StopIteration);
  return NULL;
}

PyTypeObject BPy_IDGroup_IterKeys_Type = {PyVarObject_HEAD_INIT(NULL, 0)};
PyTypeObject BPy_IDGroup_IterValues_Type = {PyVarObject_HEAD_INIT(NULL, 0)};
PyTypeObject BPy_IDGroup_IterItems_Type = {PyVarObject_HEAD_INIT(NULL, 0)};

/* ID Property Group Iterator. */
static void IDGroup_Iter_init_type(void)
{
#define SHARED_MEMBER_SET(member, value) \
  { \
    k_ty->member = v_ty->member = i_ty->member = value; \
  } \
  ((void)0)

  PyTypeObject *k_ty = &BPy_IDGroup_IterKeys_Type;
  PyTypeObject *v_ty = &BPy_IDGroup_IterValues_Type;
  PyTypeObject *i_ty = &BPy_IDGroup_IterItems_Type;

  /* Unique members. */
  k_ty->tp_name = "IDPropertyGroupIterKeys";
  v_ty->tp_name = "IDPropertyGroupIterValues";
  i_ty->tp_name = "IDPropertyGroupIterItems";

  k_ty->tp_iternext = (iternextfunc)BPy_Group_IterKeys_next;
  v_ty->tp_iternext = (iternextfunc)BPy_Group_IterValues_next;
  i_ty->tp_iternext = (iternextfunc)BPy_Group_IterItems_next;

  /* Shared members. */
  SHARED_MEMBER_SET(tp_basicsize, sizeof(BPy_IDGroup_Iter));
  SHARED_MEMBER_SET(tp_dealloc, (destructor)BPy_IDGroup_Iter_dealloc);
  SHARED_MEMBER_SET(tp_repr, (reprfunc)BPy_IDGroup_Iter_repr);
  SHARED_MEMBER_SET(tp_flags, Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC);
  SHARED_MEMBER_SET(tp_traverse, (traverseproc)BPy_IDGroup_Iter_traverse);
  SHARED_MEMBER_SET(tp_clear, (inquiry)BPy_IDGroup_Iter_clear);
  SHARED_MEMBER_SET(tp_iter, PyObject_SelfIter);

#undef SHARED_MEMBER_SET
}

static PyObject *IDGroup_Iter_New_WithType(BPy_IDProperty *group,
                                           const bool reversed,
                                           PyTypeObject *type)
{
  BLI_assert(group ? group->prop->type == IDP_GROUP : true);
  BPy_IDGroup_Iter *iter = PyObject_GC_New(BPy_IDGroup_Iter, type);
  iter->reversed = reversed;
  iter->group = group;
  if (group != NULL) {
    Py_INCREF(group);
    PyObject_GC_Track(iter);
    iter->cur = (reversed ? group->prop->data.group.last : group->prop->data.group.first);
    iter->len_init = group->prop->len;
  }
  else {
    iter->cur = NULL;
    iter->len_init = 0;
  }
  return (PyObject *)iter;
}

static PyObject *BPy_IDGroup_IterKeys_CreatePyObject(BPy_IDProperty *group, const bool reversed)
{
  return IDGroup_Iter_New_WithType(group, reversed, &BPy_IDGroup_IterKeys_Type);
}

static PyObject *BPy_IDGroup_IterValues_CreatePyObject(BPy_IDProperty *group, const bool reversed)
{
  return IDGroup_Iter_New_WithType(group, reversed, &BPy_IDGroup_IterValues_Type);
}

static PyObject *BPy_IDGroup_IterItems_CreatePyObject(BPy_IDProperty *group, const bool reversed)
{
  return IDGroup_Iter_New_WithType(group, reversed, &BPy_IDGroup_IterItems_Type);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name ID-Property Group View Types (Keys/Values/Items)
 *
 * This view types is a thin wrapper on keys/values/items, this matches Python's `dict_view` type.
 * This is returned by `property.keys()` and is separate from the iterator that loops over keys.
 *
 * There are some less common features this type could support (matching Python's `dict_view`)
 *
 * TODO:
 * - Efficient contains checks for values and items which currently convert to a list first.
 * - Missing `dict_views.isdisjoint`.
 * - Missing `tp_as_number` (`nb_subtract`, `nb_and`, `nb_xor`, `nb_or`).
 * \{ */

static PyObject *BPy_IDGroup_View_repr(BPy_IDGroup_View *self)
{
  if (self->group == NULL) {
    return PyUnicode_FromFormat("<%s>", Py_TYPE(self)->tp_name);
  }
  return PyUnicode_FromFormat("<%s \"%s\">", Py_TYPE(self)->tp_name, self->group->prop->name);
}

static void BPy_IDGroup_View_dealloc(BPy_IDGroup_View *self)
{
  if (self->group != NULL) {
    PyObject_GC_UnTrack(self);
  }
  Py_CLEAR(self->group);
  PyObject_GC_Del(self);
}

static int BPy_IDGroup_View_traverse(BPy_IDGroup_View *self, visitproc visit, void *arg)
{
  Py_VISIT(self->group);
  return 0;
}

static int BPy_IDGroup_View_clear(BPy_IDGroup_View *self)
{
  Py_CLEAR(self->group);
  return 0;
}

/* View Specific API's (Key/Value/Items). */

static PyObject *BPy_Group_ViewKeys_iter(BPy_IDGroup_View *self)
{
  return BPy_IDGroup_IterKeys_CreatePyObject(self->group, self->reversed);
}

static PyObject *BPy_Group_ViewValues_iter(BPy_IDGroup_View *self)
{
  return BPy_IDGroup_IterValues_CreatePyObject(self->group, self->reversed);
}

static PyObject *BPy_Group_ViewItems_iter(BPy_IDGroup_View *self)
{
  return BPy_IDGroup_IterItems_CreatePyObject(self->group, self->reversed);
}

static Py_ssize_t BPy_Group_View_len(BPy_IDGroup_View *self)
{
  if (self->group == NULL) {
    return 0;
  }
  return self->group->prop->len;
}

static int BPy_Group_ViewKeys_Contains(BPy_IDGroup_View *self, PyObject *value)
{
  if (self->group == NULL) {
    return 0;
  }
  return BPy_IDGroup_Contains(self->group, value);
}

static int BPy_Group_ViewValues_Contains(BPy_IDGroup_View *self, PyObject *value)
{
  if (self->group == NULL) {
    return 0;
  }
  /* TODO: implement this without first converting to a list. */
  PyObject *list = PySequence_List((PyObject *)self);
  const int result = PySequence_Contains(list, value);
  Py_DECREF(list);
  return result;
}

static int BPy_Group_ViewItems_Contains(BPy_IDGroup_View *self, PyObject *value)
{
  if (self->group == NULL) {
    return 0;
  }
  /* TODO: implement this without first converting to a list. */
  PyObject *list = PySequence_List((PyObject *)self);
  const int result = PySequence_Contains(list, value);
  Py_DECREF(list);
  return result;
}

static PySequenceMethods BPy_IDGroup_ViewKeys_as_sequence = {
    (lenfunc)BPy_Group_View_len,             /* sq_length */
    0,                                       /* sq_concat */
    0,                                       /* sq_repeat */
    0,                                       /* sq_item */
    0,                                       /* sq_slice */
    0,                                       /* sq_ass_item */
    0,                                       /* sq_ass_slice */
    (objobjproc)BPy_Group_ViewKeys_Contains, /* sq_contains */
};

static PySequenceMethods BPy_IDGroup_ViewValues_as_sequence = {
    (lenfunc)BPy_Group_View_len,               /* sq_length */
    0,                                         /* sq_concat */
    0,                                         /* sq_repeat */
    0,                                         /* sq_item */
    0,                                         /* sq_slice */
    0,                                         /* sq_ass_item */
    0,                                         /* sq_ass_slice */
    (objobjproc)BPy_Group_ViewValues_Contains, /* sq_contains */
};

static PySequenceMethods BPy_IDGroup_ViewItems_as_sequence = {
    (lenfunc)BPy_Group_View_len,              /* sq_length */
    0,                                        /* sq_concat */
    0,                                        /* sq_repeat */
    0,                                        /* sq_item */
    0,                                        /* sq_slice */
    0,                                        /* sq_ass_item */
    0,                                        /* sq_ass_slice */
    (objobjproc)BPy_Group_ViewItems_Contains, /* sq_contains */
};

/* Methods. */

PyDoc_STRVAR(BPy_IDGroup_View_reversed_doc,
             "Return a reverse iterator over the ID Property keys values or items.");

static PyObject *BPy_IDGroup_View_reversed(BPy_IDGroup_View *self, PyObject *UNUSED(ignored))
{
  BPy_IDGroup_View *result = IDGroup_View_New_WithType(self->group, Py_TYPE(self));
  result->reversed = !self->reversed;
  return (PyObject *)result;
}

static PyMethodDef BPy_IDGroup_View_methods[] = {
    {"__reversed__",
     (PyCFunction)(void (*)(void))BPy_IDGroup_View_reversed,
     METH_NOARGS,
     BPy_IDGroup_View_reversed_doc},
    {NULL, NULL},
};

PyTypeObject BPy_IDGroup_ViewKeys_Type = {PyVarObject_HEAD_INIT(NULL, 0)};
PyTypeObject BPy_IDGroup_ViewValues_Type = {PyVarObject_HEAD_INIT(NULL, 0)};
PyTypeObject BPy_IDGroup_ViewItems_Type = {PyVarObject_HEAD_INIT(NULL, 0)};

/* ID Property Group View. */
static void IDGroup_View_init_type(void)
{
  PyTypeObject *k_ty = &BPy_IDGroup_ViewKeys_Type;
  PyTypeObject *v_ty = &BPy_IDGroup_ViewValues_Type;
  PyTypeObject *i_ty = &BPy_IDGroup_ViewItems_Type;

  /* Unique members. */
  k_ty->tp_name = "IDPropertyGroupViewKeys";
  v_ty->tp_name = "IDPropertyGroupViewValues";
  i_ty->tp_name = "IDPropertyGroupViewItems";

  k_ty->tp_iter = (getiterfunc)BPy_Group_ViewKeys_iter;
  v_ty->tp_iter = (getiterfunc)BPy_Group_ViewValues_iter;
  i_ty->tp_iter = (getiterfunc)BPy_Group_ViewItems_iter;

  k_ty->tp_as_sequence = &BPy_IDGroup_ViewKeys_as_sequence;
  v_ty->tp_as_sequence = &BPy_IDGroup_ViewValues_as_sequence;
  i_ty->tp_as_sequence = &BPy_IDGroup_ViewItems_as_sequence;

  /* Shared members. */
#define SHARED_MEMBER_SET(member, value) \
  { \
    k_ty->member = v_ty->member = i_ty->member = value; \
  } \
  ((void)0)

  SHARED_MEMBER_SET(tp_basicsize, sizeof(BPy_IDGroup_View));
  SHARED_MEMBER_SET(tp_dealloc, (destructor)BPy_IDGroup_View_dealloc);
  SHARED_MEMBER_SET(tp_repr, (reprfunc)BPy_IDGroup_View_repr);
  SHARED_MEMBER_SET(tp_flags, Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC);
  SHARED_MEMBER_SET(tp_traverse, (traverseproc)BPy_IDGroup_View_traverse);
  SHARED_MEMBER_SET(tp_clear, (inquiry)BPy_IDGroup_View_clear);
  SHARED_MEMBER_SET(tp_methods, BPy_IDGroup_View_methods);

#undef SHARED_MEMBER_SET
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name ID-Property Group Methods
 * \{ */

PyDoc_STRVAR(
    BPy_IDGroup_pop_doc,
    ".. method:: pop(key, default)\n"
    "\n"
    "   Remove an item from the group, returning a Python representation.\n"
    "\n"
    "   :raises KeyError: When the item doesn't exist.\n"
    "\n"
    "   :arg key: Name of item to remove.\n"
    "   :type key: string\n"
    "   :arg default: Value to return when key isn't found, otherwise raise an exception.\n"
    "   :type default: Undefined\n");
static PyObject *BPy_IDGroup_pop(BPy_IDProperty *self, PyObject *args)
{
  IDProperty *idprop;
  PyObject *pyform;

  char *key;
  PyObject *def = NULL;

  if (!PyArg_ParseTuple(args, "s|O:get", &key, &def)) {
    return NULL;
  }

  idprop = IDP_GetPropertyFromGroup(self->prop, key);
  if (idprop == NULL) {
    if (def == NULL) {
      PyErr_SetString(PyExc_KeyError, "item not in group");
      return NULL;
    }
    return Py_INCREF_RET(def);
  }

  pyform = BPy_IDGroup_MapDataToPy(idprop);
  if (pyform == NULL) {
    /* Ok something bad happened with the #PyObject, so don't remove the prop from the group.
     * if `pyform` is NULL, then it already should have raised an exception. */
    return NULL;
  }

  IDP_FreeFromGroup(self->prop, idprop);
  return pyform;
}

/* utility function */
static void BPy_IDGroup_CorrectListLen(IDProperty *prop, PyObject *seq, int len, const char *func)
{
  int j;

  printf("%s: ID Property Error found and corrected!\n", func);

  /* fill rest of list with valid references to None */
  for (j = len; j < prop->len; j++) {
    PyList_SET_ITEM(seq, j, Py_INCREF_RET(Py_None));
  }

  /* Set correct group length. */
  prop->len = len;
}

PyObject *BPy_Wrap_GetKeys(IDProperty *prop)
{
  PyObject *list = PyList_New(prop->len);
  IDProperty *loop;
  int i;

  for (i = 0, loop = prop->data.group.first; loop && (i < prop->len); loop = loop->next, i++) {
    PyList_SET_ITEM(list, i, PyUnicode_FromString(loop->name));
  }

  /* if the id prop is corrupt, count the remaining */
  for (; loop; loop = loop->next, i++) {
    /* pass */
  }

  if (i != prop->len) { /* if the loop didn't finish, we know the length is wrong */
    BPy_IDGroup_CorrectListLen(prop, list, i, __func__);
    Py_DECREF(list); /* Free the list. */
    /* Call self again. */
    return BPy_Wrap_GetKeys(prop);
  }

  return list;
}

PyObject *BPy_Wrap_GetValues(ID *id, IDProperty *prop)
{
  PyObject *list = PyList_New(prop->len);
  IDProperty *loop;
  int i;

  for (i = 0, loop = prop->data.group.first; loop; loop = loop->next, i++) {
    PyList_SET_ITEM(list, i, BPy_IDGroup_WrapData(id, loop, prop));
  }

  if (i != prop->len) {
    BPy_IDGroup_CorrectListLen(prop, list, i, __func__);
    Py_DECREF(list); /* Free the list. */
    /* Call self again. */
    return BPy_Wrap_GetValues(id, prop);
  }

  return list;
}

PyObject *BPy_Wrap_GetItems(ID *id, IDProperty *prop)
{
  PyObject *seq = PyList_New(prop->len);
  IDProperty *loop;
  int i;

  for (i = 0, loop = prop->data.group.first; loop; loop = loop->next, i++) {
    PyObject *item = PyTuple_New(2);
    PyTuple_SET_ITEMS(
        item, PyUnicode_FromString(loop->name), BPy_IDGroup_WrapData(id, loop, prop));
    PyList_SET_ITEM(seq, i, item);
  }

  if (i != prop->len) {
    BPy_IDGroup_CorrectListLen(prop, seq, i, __func__);
    Py_DECREF(seq); /* Free the list. */
    /* Call self again. */
    return BPy_Wrap_GetItems(id, prop);
  }

  return seq;
}

PyObject *BPy_Wrap_GetKeys_View_WithID(ID *id, IDProperty *prop)
{
  PyObject *self = prop ? idprop_py_from_idp_group(id, prop, NULL) : NULL;
  PyObject *ret = BPy_IDGroup_ViewKeys_CreatePyObject((BPy_IDProperty *)self);
  Py_XDECREF(self); /* Owned by `ret`. */
  return ret;
}

PyObject *BPy_Wrap_GetValues_View_WithID(ID *id, IDProperty *prop)
{
  PyObject *self = prop ? idprop_py_from_idp_group(id, prop, NULL) : NULL;
  PyObject *ret = BPy_IDGroup_ViewValues_CreatePyObject((BPy_IDProperty *)self);
  Py_XDECREF(self); /* Owned by `ret`. */
  return ret;
}

PyObject *BPy_Wrap_GetItems_View_WithID(ID *id, IDProperty *prop)
{
  PyObject *self = prop ? idprop_py_from_idp_group(id, prop, NULL) : NULL;
  PyObject *ret = BPy_IDGroup_ViewItems_CreatePyObject((BPy_IDProperty *)self);
  Py_XDECREF(self); /* Owned by `ret`. */
  return ret;
}

PyDoc_STRVAR(BPy_IDGroup_keys_doc,
             ".. method:: keys()\n"
             "\n"
             "   Return the keys associated with this group as a list of strings.\n");
static PyObject *BPy_IDGroup_keys(BPy_IDProperty *self)
{
  return BPy_IDGroup_ViewKeys_CreatePyObject(self);
}

PyDoc_STRVAR(BPy_IDGroup_values_doc,
             ".. method:: values()\n"
             "\n"
             "   Return the values associated with this group.\n");
static PyObject *BPy_IDGroup_values(BPy_IDProperty *self)
{
  return BPy_IDGroup_ViewValues_CreatePyObject(self);
}

PyDoc_STRVAR(BPy_IDGroup_items_doc,
             ".. method:: items()\n"
             "\n"
             "   Iterate through the items in the dict; behaves like dictionary method items.\n");
static PyObject *BPy_IDGroup_items(BPy_IDProperty *self)
{
  return BPy_IDGroup_ViewItems_CreatePyObject(self);
}

static int BPy_IDGroup_Contains(BPy_IDProperty *self, PyObject *value)
{
  const char *name = PyUnicode_AsUTF8(value);

  if (!name) {
    PyErr_Format(PyExc_TypeError, "expected a string, not a %.200s", Py_TYPE(value)->tp_name);
    return -1;
  }

  return IDP_GetPropertyFromGroup(self->prop, name) ? 1 : 0;
}

PyDoc_STRVAR(BPy_IDGroup_update_doc,
             ".. method:: update(other)\n"
             "\n"
             "   Update key, values.\n"
             "\n"
             "   :arg other: Updates the values in the group with this.\n"
             "   :type other: :class:`IDPropertyGroup` or dict\n");
static PyObject *BPy_IDGroup_update(BPy_IDProperty *self, PyObject *value)
{
  PyObject *pkey, *pval;
  Py_ssize_t i = 0;

  if (BPy_IDGroup_Check(value)) {
    BPy_IDProperty *other = (BPy_IDProperty *)value;
    if (UNLIKELY(self->prop == other->prop)) {
      Py_RETURN_NONE;
    }

    /* XXX, possible one is inside the other */
    IDP_MergeGroup(self->prop, other->prop, true);
  }
  else if (PyDict_Check(value)) {
    while (PyDict_Next(value, &i, &pkey, &pval)) {
      BPy_IDGroup_Map_SetItem(self, pkey, pval);
      if (PyErr_Occurred()) {
        return NULL;
      }
    }
  }
  else {
    PyErr_Format(PyExc_TypeError,
                 "expected a dict or an IDPropertyGroup type, not a %.200s",
                 Py_TYPE(value)->tp_name);
    return NULL;
  }

  Py_RETURN_NONE;
}

PyDoc_STRVAR(BPy_IDGroup_to_dict_doc,
             ".. method:: to_dict()\n"
             "\n"
             "   Return a purely python version of the group.\n");
static PyObject *BPy_IDGroup_to_dict(BPy_IDProperty *self)
{
  return BPy_IDGroup_MapDataToPy(self->prop);
}

PyDoc_STRVAR(BPy_IDGroup_clear_doc,
             ".. method:: clear()\n"
             "\n"
             "   Clear all members from this group.\n");
static PyObject *BPy_IDGroup_clear(BPy_IDProperty *self)
{
  IDP_ClearProperty(self->prop);
  Py_RETURN_NONE;
}

PyDoc_STRVAR(BPy_IDGroup_get_doc,
             ".. method:: get(key, default=None)\n"
             "\n"
             "   Return the value for key, if it exists, else default.\n");
static PyObject *BPy_IDGroup_get(BPy_IDProperty *self, PyObject *args)
{
  IDProperty *idprop;
  const char *key;
  PyObject *def = Py_None;

  if (!PyArg_ParseTuple(args, "s|O:get", &key, &def)) {
    return NULL;
  }

  idprop = IDP_GetPropertyFromGroup(self->prop, key);
  if (idprop) {
    PyObject *pyobj = BPy_IDGroup_WrapData(self->owner_id, idprop, self->prop);
    if (pyobj) {
      return pyobj;
    }
  }

  Py_INCREF(def);
  return def;
}

static struct PyMethodDef BPy_IDGroup_methods[] = {
    {"pop", (PyCFunction)BPy_IDGroup_pop, METH_VARARGS, BPy_IDGroup_pop_doc},
    {"keys", (PyCFunction)BPy_IDGroup_keys, METH_NOARGS, BPy_IDGroup_keys_doc},
    {"values", (PyCFunction)BPy_IDGroup_values, METH_NOARGS, BPy_IDGroup_values_doc},
    {"items", (PyCFunction)BPy_IDGroup_items, METH_NOARGS, BPy_IDGroup_items_doc},
    {"update", (PyCFunction)BPy_IDGroup_update, METH_O, BPy_IDGroup_update_doc},
    {"get", (PyCFunction)BPy_IDGroup_get, METH_VARARGS, BPy_IDGroup_get_doc},
    {"to_dict", (PyCFunction)BPy_IDGroup_to_dict, METH_NOARGS, BPy_IDGroup_to_dict_doc},
    {"clear", (PyCFunction)BPy_IDGroup_clear, METH_NOARGS, BPy_IDGroup_clear_doc},
    {NULL, NULL, 0, NULL},
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name ID-Property Group Type
 * \{ */

static PySequenceMethods BPy_IDGroup_Seq = {
    (lenfunc)BPy_IDGroup_Map_Len, /* lenfunc sq_length */
    NULL,                         /* binaryfunc sq_concat */
    NULL,                         /* ssizeargfunc sq_repeat */
    NULL,
    /* ssizeargfunc sq_item */ /* TODO: setting this will allow PySequence_Check to return True. */
    NULL,                      /* intintargfunc ***was_sq_slice*** */
    NULL,                      /* intobjargproc sq_ass_item */
    NULL,                      /* ssizeobjargproc ***was_sq_ass_slice*** */
    (objobjproc)BPy_IDGroup_Contains, /* objobjproc sq_contains */
    NULL,                             /* binaryfunc sq_inplace_concat */
    NULL,                             /* ssizeargfunc sq_inplace_repeat */
};

static PyMappingMethods BPy_IDGroup_Mapping = {
    (lenfunc)BPy_IDGroup_Map_Len,           /* inquiry mp_length */
    (binaryfunc)BPy_IDGroup_Map_GetItem,    /* binaryfunc mp_subscript */
    (objobjargproc)BPy_IDGroup_Map_SetItem, /* objobjargproc mp_ass_subscript */
};

PyTypeObject BPy_IDGroup_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    /*  For printing, in format "<module>.<name>" */
    "IDPropertyGroup",      /* char *tp_name; */
    sizeof(BPy_IDProperty), /* int tp_basicsize; */
    0,                      /* tp_itemsize;  For allocation */

    /* Methods to implement standard operations */

    NULL,                       /* destructor tp_dealloc; */
    0,                          /* tp_vectorcall_offset */
    NULL,                       /* getattrfunc tp_getattr; */
    NULL,                       /* setattrfunc tp_setattr; */
    NULL,                       /* cmpfunc tp_compare; */
    (reprfunc)BPy_IDGroup_repr, /* reprfunc tp_repr; */

    /* Method suites for standard classes */

    NULL,                 /* PyNumberMethods *tp_as_number; */
    &BPy_IDGroup_Seq,     /* PySequenceMethods *tp_as_sequence; */
    &BPy_IDGroup_Mapping, /* PyMappingMethods *tp_as_mapping; */

    /* More standard operations (here for binary compatibility) */

    (hashfunc)BPy_IDGroup_hash, /* hashfunc tp_hash; */
    NULL,                       /* ternaryfunc tp_call; */
    NULL,                       /* reprfunc tp_str; */
    NULL,                       /* getattrofunc tp_getattro; */
    NULL,                       /* setattrofunc tp_setattro; */

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
    (getiterfunc)BPy_IDGroup_iter, /* getiterfunc tp_iter; */
    NULL,                          /* iternextfunc tp_iternext; */
    /*** Attribute descriptor and subclassing stuff ***/
    BPy_IDGroup_methods,   /* struct PyMethodDef *tp_methods; */
    NULL,                  /* struct PyMemberDef *tp_members; */
    BPy_IDGroup_getseters, /* struct PyGetSetDef *tp_getset; */
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name ID Array Methods
 * \{ */

static PyTypeObject *idp_array_py_type(BPy_IDArray *self, bool *r_is_double)
{
  switch (self->prop->subtype) {
    case IDP_FLOAT:
      *r_is_double = false;
      return &PyFloat_Type;
    case IDP_DOUBLE:
      *r_is_double = true;
      return &PyFloat_Type;
    case IDP_INT:
      *r_is_double = false;
      return &PyLong_Type;
    default:
      *r_is_double = false;
      return NULL;
  }
}

static PyObject *BPy_IDArray_repr(BPy_IDArray *self)
{
  return PyUnicode_FromFormat("<bpy id property array [%d]>", self->prop->len);
}

PyDoc_STRVAR(BPy_IDArray_get_typecode_doc,
             "The type of the data in the array {'f': float, 'd': double, 'i': int}.");
static PyObject *BPy_IDArray_get_typecode(BPy_IDArray *self)
{
  switch (self->prop->subtype) {
    case IDP_FLOAT:
      return PyUnicode_FromString("f");
    case IDP_DOUBLE:
      return PyUnicode_FromString("d");
    case IDP_INT:
      return PyUnicode_FromString("i");
  }

  PyErr_Format(
      PyExc_RuntimeError, "%s: invalid/corrupt array type '%d'!", __func__, self->prop->subtype);

  return NULL;
}

static PyGetSetDef BPy_IDArray_getseters[] = {
    /* matches pythons array.typecode */
    {"typecode",
     (getter)BPy_IDArray_get_typecode,
     (setter)NULL,
     BPy_IDArray_get_typecode_doc,
     NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

PyDoc_STRVAR(BPy_IDArray_to_list_doc,
             ".. method:: to_list()\n"
             "\n"
             "   Return the array as a list.\n");
static PyObject *BPy_IDArray_to_list(BPy_IDArray *self)
{
  return BPy_IDGroup_MapDataToPy(self->prop);
}

static PyMethodDef BPy_IDArray_methods[] = {
    {"to_list", (PyCFunction)BPy_IDArray_to_list, METH_NOARGS, BPy_IDArray_to_list_doc},
    {NULL, NULL, 0, NULL},
};

static int BPy_IDArray_Len(BPy_IDArray *self)
{
  return self->prop->len;
}

static PyObject *BPy_IDArray_GetItem(BPy_IDArray *self, int index)
{
  if (index < 0 || index >= self->prop->len) {
    PyErr_SetString(PyExc_IndexError, "index out of range!");
    return NULL;
  }

  switch (self->prop->subtype) {
    case IDP_FLOAT:
      return PyFloat_FromDouble(((float *)IDP_Array(self->prop))[index]);
    case IDP_DOUBLE:
      return PyFloat_FromDouble(((double *)IDP_Array(self->prop))[index]);
    case IDP_INT:
      return PyLong_FromLong((long)((int *)IDP_Array(self->prop))[index]);
  }

  PyErr_Format(
      PyExc_RuntimeError, "%s: invalid/corrupt array type '%d'!", __func__, self->prop->subtype);

  return NULL;
}

static int BPy_IDArray_SetItem(BPy_IDArray *self, int index, PyObject *value)
{
  if (index < 0 || index >= self->prop->len) {
    PyErr_SetString(PyExc_RuntimeError, "index out of range!");
    return -1;
  }

  switch (self->prop->subtype) {
    case IDP_FLOAT: {
      const float f = (float)PyFloat_AsDouble(value);
      if (f == -1 && PyErr_Occurred()) {
        return -1;
      }
      ((float *)IDP_Array(self->prop))[index] = f;
      break;
    }
    case IDP_DOUBLE: {
      const double d = PyFloat_AsDouble(value);
      if (d == -1 && PyErr_Occurred()) {
        return -1;
      }
      ((double *)IDP_Array(self->prop))[index] = d;
      break;
    }
    case IDP_INT: {
      const int i = PyC_Long_AsI32(value);
      if (i == -1 && PyErr_Occurred()) {
        return -1;
      }

      ((int *)IDP_Array(self->prop))[index] = i;
      break;
    }
  }
  return 0;
}

static PySequenceMethods BPy_IDArray_Seq = {
    (lenfunc)BPy_IDArray_Len,             /* inquiry sq_length */
    NULL,                                 /* binaryfunc sq_concat */
    NULL,                                 /* intargfunc sq_repeat */
    (ssizeargfunc)BPy_IDArray_GetItem,    /* intargfunc sq_item */
    NULL,                                 /* intintargfunc sq_slice */
    (ssizeobjargproc)BPy_IDArray_SetItem, /* intobjargproc sq_ass_item */
    NULL,                                 /* intintobjargproc sq_ass_slice */
    NULL,                                 /* objobjproc sq_contains */
    /* Added in release 2.0 */
    NULL, /* binaryfunc sq_inplace_concat */
    NULL, /* intargfunc sq_inplace_repeat */
};

/* sequence slice (get): idparr[a:b] */
static PyObject *BPy_IDArray_slice(BPy_IDArray *self, int begin, int end)
{
  IDProperty *prop = self->prop;
  PyObject *tuple;
  int count;

  CLAMP(begin, 0, prop->len);
  if (end < 0) {
    end = prop->len + end + 1;
  }
  CLAMP(end, 0, prop->len);
  begin = MIN2(begin, end);

  tuple = PyTuple_New(end - begin);

  switch (prop->subtype) {
    case IDP_FLOAT: {
      const float *array = (float *)IDP_Array(prop);
      for (count = begin; count < end; count++) {
        PyTuple_SET_ITEM(tuple, count - begin, PyFloat_FromDouble(array[count]));
      }
      break;
    }
    case IDP_DOUBLE: {
      const double *array = (double *)IDP_Array(prop);
      for (count = begin; count < end; count++) {
        PyTuple_SET_ITEM(tuple, count - begin, PyFloat_FromDouble(array[count]));
      }
      break;
    }
    case IDP_INT: {
      const int *array = (int *)IDP_Array(prop);
      for (count = begin; count < end; count++) {
        PyTuple_SET_ITEM(tuple, count - begin, PyLong_FromLong(array[count]));
      }
      break;
    }
  }

  return tuple;
}
/* sequence slice (set): idparr[a:b] = value */
static int BPy_IDArray_ass_slice(BPy_IDArray *self, int begin, int end, PyObject *seq)
{
  IDProperty *prop = self->prop;
  bool is_double;
  const PyTypeObject *py_type = idp_array_py_type(self, &is_double);
  const size_t elem_size = is_double ? sizeof(double) : sizeof(float);
  size_t alloc_len;
  size_t size;
  void *vec;

  CLAMP(begin, 0, prop->len);
  CLAMP(end, 0, prop->len);
  begin = MIN2(begin, end);

  size = (end - begin);
  alloc_len = size * elem_size;

  /* NOTE: we count on int/float being the same size here */
  vec = MEM_mallocN(alloc_len, "array assignment");

  if (PyC_AsArray(vec, elem_size, seq, size, py_type, "slice assignment: ") == -1) {
    MEM_freeN(vec);
    return -1;
  }

  memcpy((void *)(((char *)IDP_Array(prop)) + (begin * elem_size)), vec, alloc_len);

  MEM_freeN(vec);
  return 0;
}

static PyObject *BPy_IDArray_subscript(BPy_IDArray *self, PyObject *item)
{
  if (PyIndex_Check(item)) {
    Py_ssize_t i;
    i = PyNumber_AsSsize_t(item, PyExc_IndexError);
    if (i == -1 && PyErr_Occurred()) {
      return NULL;
    }
    if (i < 0) {
      i += self->prop->len;
    }
    return BPy_IDArray_GetItem(self, i);
  }
  if (PySlice_Check(item)) {
    Py_ssize_t start, stop, step, slicelength;

    if (PySlice_GetIndicesEx(item, self->prop->len, &start, &stop, &step, &slicelength) < 0) {
      return NULL;
    }

    if (slicelength <= 0) {
      return PyTuple_New(0);
    }
    if (step == 1) {
      return BPy_IDArray_slice(self, start, stop);
    }

    PyErr_SetString(PyExc_TypeError, "slice steps not supported with vectors");
    return NULL;
  }

  PyErr_Format(PyExc_TypeError,
               "vector indices must be integers, not %.200s",
               __func__,
               Py_TYPE(item)->tp_name);
  return NULL;
}

static int BPy_IDArray_ass_subscript(BPy_IDArray *self, PyObject *item, PyObject *value)
{
  if (PyIndex_Check(item)) {
    Py_ssize_t i = PyNumber_AsSsize_t(item, PyExc_IndexError);
    if (i == -1 && PyErr_Occurred()) {
      return -1;
    }
    if (i < 0) {
      i += self->prop->len;
    }
    return BPy_IDArray_SetItem(self, i, value);
  }
  if (PySlice_Check(item)) {
    Py_ssize_t start, stop, step, slicelength;

    if (PySlice_GetIndicesEx(item, self->prop->len, &start, &stop, &step, &slicelength) < 0) {
      return -1;
    }

    if (step == 1) {
      return BPy_IDArray_ass_slice(self, start, stop, value);
    }

    PyErr_SetString(PyExc_TypeError, "slice steps not supported with vectors");
    return -1;
  }

  PyErr_Format(
      PyExc_TypeError, "vector indices must be integers, not %.200s", Py_TYPE(item)->tp_name);
  return -1;
}

static PyMappingMethods BPy_IDArray_AsMapping = {
    (lenfunc)BPy_IDArray_Len,
    (binaryfunc)BPy_IDArray_subscript,
    (objobjargproc)BPy_IDArray_ass_subscript,
};

static int itemsize_by_idarray_type(int array_type)
{
  if (array_type == IDP_INT) {
    return sizeof(int);
  }
  if (array_type == IDP_FLOAT) {
    return sizeof(float);
  }
  if (array_type == IDP_DOUBLE) {
    return sizeof(double);
  }
  return -1; /* should never happen */
}

static int BPy_IDArray_getbuffer(BPy_IDArray *self, Py_buffer *view, int flags)
{
  IDProperty *prop = self->prop;
  const int itemsize = itemsize_by_idarray_type(prop->subtype);
  const int length = itemsize * prop->len;

  if (PyBuffer_FillInfo(view, (PyObject *)self, IDP_Array(prop), length, false, flags) == -1) {
    return -1;
  }

  view->itemsize = itemsize;
  view->format = (char *)idp_format_from_array_type(prop->subtype);

  Py_ssize_t *shape = MEM_mallocN(sizeof(Py_ssize_t), __func__);
  shape[0] = prop->len;
  view->shape = shape;

  return 0;
}

static void BPy_IDArray_releasebuffer(BPy_IDArray *UNUSED(self), Py_buffer *view)
{
  MEM_freeN(view->shape);
}

static PyBufferProcs BPy_IDArray_Buffer = {
    (getbufferproc)BPy_IDArray_getbuffer,
    (releasebufferproc)BPy_IDArray_releasebuffer,
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name ID Array Type
 * \{ */

PyTypeObject BPy_IDArray_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    /*  For printing, in format "<module>.<name>" */
    "IDPropertyArray",   /* char *tp_name; */
    sizeof(BPy_IDArray), /* int tp_basicsize; */
    0,                   /* tp_itemsize;  For allocation */

    /* Methods to implement standard operations */

    NULL,                       /* destructor tp_dealloc; */
    0,                          /* tp_vectorcall_offset */
    NULL,                       /* getattrfunc tp_getattr; */
    NULL,                       /* setattrfunc tp_setattr; */
    NULL,                       /* cmpfunc tp_compare; */
    (reprfunc)BPy_IDArray_repr, /* reprfunc tp_repr; */

    /* Method suites for standard classes */

    NULL,                   /* PyNumberMethods *tp_as_number; */
    &BPy_IDArray_Seq,       /* PySequenceMethods *tp_as_sequence; */
    &BPy_IDArray_AsMapping, /* PyMappingMethods *tp_as_mapping; */

    /* More standard operations (here for binary compatibility) */

    NULL, /* hashfunc tp_hash; */
    NULL, /* ternaryfunc tp_call; */
    NULL, /* reprfunc tp_str; */
    NULL, /* getattrofunc tp_getattro; */
    NULL, /* setattrofunc tp_setattro; */

    /* Functions to access object as input/output buffer */
    &BPy_IDArray_Buffer, /* PyBufferProcs *tp_as_buffer; */

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
    BPy_IDArray_methods,   /* struct PyMethodDef *tp_methods; */
    NULL,                  /* struct PyMemberDef *tp_members; */
    BPy_IDArray_getseters, /* struct PyGetSetDef *tp_getset; */
    NULL,                  /* struct _typeobject *tp_base; */
    NULL,                  /* PyObject *tp_dict; */
    NULL,                  /* descrgetfunc tp_descr_get; */
    NULL,                  /* descrsetfunc tp_descr_set; */
    0,                     /* long tp_dictoffset; */
    NULL,                  /* initproc tp_init; */
    NULL,                  /* allocfunc tp_alloc; */
    NULL,                  /* newfunc tp_new; */
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
/** \name Initialize Types
 * \{ */

void IDProp_Init_Types(void)
{
  IDGroup_Iter_init_type();
  IDGroup_View_init_type();

  PyType_Ready(&BPy_IDGroup_Type);
  PyType_Ready(&BPy_IDArray_Type);

  PyType_Ready(&BPy_IDGroup_IterKeys_Type);
  PyType_Ready(&BPy_IDGroup_IterValues_Type);
  PyType_Ready(&BPy_IDGroup_IterItems_Type);

  PyType_Ready(&BPy_IDGroup_ViewKeys_Type);
  PyType_Ready(&BPy_IDGroup_ViewValues_Type);
  PyType_Ready(&BPy_IDGroup_ViewItems_Type);
}

/**
 * \note `group` may be NULL, unlike most other uses of this argument.
 * This is supported so RNA keys/values/items methods returns an iterator with the expected type:
 * - Without having ID-properties.
 * - Without supporting #BPy_IDProperty.prop being NULL, which would incur many more checks.
 * Python's own dictionary-views also works this way too.
 */
static BPy_IDGroup_View *IDGroup_View_New_WithType(BPy_IDProperty *group, PyTypeObject *type)
{
  BLI_assert(group ? group->prop->type == IDP_GROUP : true);
  BPy_IDGroup_View *iter = PyObject_GC_New(BPy_IDGroup_View, type);
  iter->reversed = false;
  iter->group = group;
  if (group != NULL) {
    Py_INCREF(group);
    PyObject_GC_Track(iter);
  }
  return iter;
}

static PyObject *BPy_IDGroup_ViewKeys_CreatePyObject(BPy_IDProperty *group)
{
  return (PyObject *)IDGroup_View_New_WithType(group, &BPy_IDGroup_ViewKeys_Type);
}

static PyObject *BPy_IDGroup_ViewValues_CreatePyObject(BPy_IDProperty *group)
{
  return (PyObject *)IDGroup_View_New_WithType(group, &BPy_IDGroup_ViewValues_Type);
}

static PyObject *BPy_IDGroup_ViewItems_CreatePyObject(BPy_IDProperty *group)
{
  return (PyObject *)IDGroup_View_New_WithType(group, &BPy_IDGroup_ViewItems_Type);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Public Module 'idprop.types'
 * \{ */

static struct PyModuleDef IDProp_types_module_def = {
    PyModuleDef_HEAD_INIT,
    "idprop.types", /* m_name */
    NULL,           /* m_doc */
    0,              /* m_size */
    NULL,           /* m_methods */
    NULL,           /* m_reload */
    NULL,           /* m_traverse */
    NULL,           /* m_clear */
    NULL,           /* m_free */
};

static PyObject *BPyInit_idprop_types(void)
{
  PyObject *submodule;

  submodule = PyModule_Create(&IDProp_types_module_def);

  IDProp_Init_Types();
  IDPropertyUIData_Init_Types();

  /* bmesh_py_types.c */
  PyModule_AddType(submodule, &BPy_IDGroup_Type);

  PyModule_AddType(submodule, &BPy_IDGroup_ViewKeys_Type);
  PyModule_AddType(submodule, &BPy_IDGroup_ViewValues_Type);
  PyModule_AddType(submodule, &BPy_IDGroup_ViewItems_Type);

  PyModule_AddType(submodule, &BPy_IDGroup_IterKeys_Type);
  PyModule_AddType(submodule, &BPy_IDGroup_IterValues_Type);
  PyModule_AddType(submodule, &BPy_IDGroup_IterItems_Type);

  PyModule_AddType(submodule, &BPy_IDArray_Type);

  return submodule;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Public Module 'idprop'
 * \{ */

static PyMethodDef IDProp_methods[] = {
    {NULL, NULL, 0, NULL},
};

PyDoc_STRVAR(IDProp_module_doc,
             "This module provides access id property types (currently mainly for docs).");
static struct PyModuleDef IDProp_module_def = {
    PyModuleDef_HEAD_INIT,
    "idprop",          /* m_name */
    IDProp_module_doc, /* m_doc */
    0,                 /* m_size */
    IDProp_methods,    /* m_methods */
    NULL,              /* m_reload */
    NULL,              /* m_traverse */
    NULL,              /* m_clear */
    NULL,              /* m_free */
};

PyObject *BPyInit_idprop(void)
{
  PyObject *mod;
  PyObject *submodule;
  PyObject *sys_modules = PyImport_GetModuleDict();

  mod = PyModule_Create(&IDProp_module_def);

  /* idprop.types */
  PyModule_AddObject(mod, "types", (submodule = BPyInit_idprop_types()));
  PyDict_SetItem(sys_modules, PyModule_GetNameObject(submodule), submodule);

  return mod;
}

/** \} */
