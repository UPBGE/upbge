/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup pythonintern
 */

#include "BLI_utildefines.h"
#include <Python.h>

#include "bpy_app_ocio.h"

#include "../generic/py_capi_utils.h"

#ifdef WITH_OCIO
#  include "ocio_capi.h"
#endif

static PyTypeObject BlenderAppOCIOType;

static PyStructSequence_Field app_ocio_info_fields[] = {
    {"supported", "Boolean, True when Blender is built with OpenColorIO support"},
    {"version", "The OpenColorIO version as a tuple of 3 numbers"},
    {"version_string", "The OpenColorIO version formatted as a string"},
    {NULL},
};

static PyStructSequence_Desc app_ocio_info_desc = {
    /* name */
    "bpy.app.ocio",
    /* doc */
    "This module contains information about OpenColorIO blender is linked against",
    /* fields */
    app_ocio_info_fields,
    ARRAY_SIZE(app_ocio_info_fields) - 1,
};

static PyObject *make_ocio_info(void)
{
  PyObject *ocio_info;
  int pos = 0;

#ifdef WITH_OCIO
  int curversion;
#endif

  ocio_info = PyStructSequence_New(&BlenderAppOCIOType);
  if (ocio_info == NULL) {
    return NULL;
  }

#ifndef WITH_OCIO
#  define SetStrItem(str) PyStructSequence_SET_ITEM(ocio_info, pos++, PyUnicode_FromString(str))
#endif

#define SetObjItem(obj) PyStructSequence_SET_ITEM(ocio_info, pos++, obj)

#ifdef WITH_OCIO
  curversion = OCIO_getVersionHex();
  SetObjItem(PyBool_FromLong(1));
  SetObjItem(
      PyC_Tuple_Pack_I32(curversion >> 24, (curversion >> 16) % 256, (curversion >> 8) % 256));
  SetObjItem(PyUnicode_FromFormat(
      "%2d, %2d, %2d", curversion >> 24, (curversion >> 16) % 256, (curversion >> 8) % 256));
#else
  SetObjItem(PyBool_FromLong(0));
  SetObjItem(PyC_Tuple_Pack_I32(0, 0, 0));
  SetStrItem("Unknown");
#endif

  if (UNLIKELY(PyErr_Occurred())) {
    Py_DECREF(ocio_info);
    return NULL;
  }

#undef SetStrItem
#undef SetObjItem

  return ocio_info;
}

PyObject *BPY_app_ocio_struct(void)
{
  PyObject *ret;

  PyStructSequence_InitType(&BlenderAppOCIOType, &app_ocio_info_desc);

  ret = make_ocio_info();

  /* prevent user from creating new instances */
  BlenderAppOCIOType.tp_init = NULL;
  BlenderAppOCIOType.tp_new = NULL;
  BlenderAppOCIOType.tp_hash = (hashfunc)
      _Py_HashPointer; /* without this we can't do set(sys.modules) T29635. */

  return ret;
}
