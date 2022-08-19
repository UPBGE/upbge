/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup pythonintern
 *
 * This file contains utility functions for getting data from a python stack
 * trace.
 */

#include <Python.h>
#include <frameobject.h>

#include "BLI_path_util.h"
#include "BLI_utildefines.h"
#ifdef WIN32
#  include "BLI_string.h" /* BLI_strcasecmp */
#endif

#include "bpy_traceback.h"

static const char *traceback_filepath(PyTracebackObject *tb, PyObject **coerce)
{
  PyCodeObject *code = PyFrame_GetCode(tb->tb_frame);
  *coerce = PyUnicode_EncodeFSDefault(code->co_filename);
  return PyBytes_AS_STRING(*coerce);
}

/* copied from pythonrun.c, 3.10.0 */
_Py_static_string(PyId_string, "<string>");

static int parse_syntax_error(PyObject *err,
                              PyObject **message,
                              PyObject **filename,
                              int *lineno,
                              int *offset,
                              int *end_lineno,
                              int *end_offset,
                              PyObject **text)
{
  Py_ssize_t hold;
  PyObject *v;
  _Py_IDENTIFIER(msg);
  _Py_IDENTIFIER(filename);
  _Py_IDENTIFIER(lineno);
  _Py_IDENTIFIER(offset);
  _Py_IDENTIFIER(end_lineno);
  _Py_IDENTIFIER(end_offset);
  _Py_IDENTIFIER(text);

  *message = NULL;
  *filename = NULL;

  /* new style errors.  `err' is an instance */
  *message = _PyObject_GetAttrId(err, &PyId_msg);
  if (!*message) {
    goto finally;
  }

  v = _PyObject_GetAttrId(err, &PyId_filename);
  if (!v) {
    goto finally;
  }
  if (v == Py_None) {
    Py_DECREF(v);
    *filename = _PyUnicode_FromId(&PyId_string);
    if (*filename == NULL) {
      goto finally;
    }
    Py_INCREF(*filename);
  }
  else {
    *filename = v;
  }

  v = _PyObject_GetAttrId(err, &PyId_lineno);
  if (!v) {
    goto finally;
  }
  hold = PyLong_AsSsize_t(v);
  Py_DECREF(v);
  if (hold < 0 && PyErr_Occurred()) {
    goto finally;
  }
  *lineno = (int)hold;

  v = _PyObject_GetAttrId(err, &PyId_offset);
  if (!v) {
    goto finally;
  }
  if (v == Py_None) {
    *offset = -1;
    Py_DECREF(v);
  }
  else {
    hold = PyLong_AsSsize_t(v);
    Py_DECREF(v);
    if (hold < 0 && PyErr_Occurred()) {
      goto finally;
    }
    *offset = (int)hold;
  }

  if (Py_TYPE(err) == (PyTypeObject *)PyExc_SyntaxError) {
    v = _PyObject_GetAttrId(err, &PyId_end_lineno);
    if (!v) {
      PyErr_Clear();
      *end_lineno = *lineno;
    }
    else if (v == Py_None) {
      *end_lineno = *lineno;
      Py_DECREF(v);
    }
    else {
      hold = PyLong_AsSsize_t(v);
      Py_DECREF(v);
      if (hold < 0 && PyErr_Occurred()) {
        goto finally;
      }
      *end_lineno = hold;
    }

    v = _PyObject_GetAttrId(err, &PyId_end_offset);
    if (!v) {
      PyErr_Clear();
      *end_offset = -1;
    }
    else if (v == Py_None) {
      *end_offset = -1;
      Py_DECREF(v);
    }
    else {
      hold = PyLong_AsSsize_t(v);
      Py_DECREF(v);
      if (hold < 0 && PyErr_Occurred()) {
        goto finally;
      }
      *end_offset = hold;
    }
  }
  else {
    /* `SyntaxError` subclasses. */
    *end_lineno = *lineno;
    *end_offset = -1;
  }

  v = _PyObject_GetAttrId(err, &PyId_text);
  if (!v) {
    goto finally;
  }
  if (v == Py_None) {
    Py_DECREF(v);
    *text = NULL;
  }
  else {
    *text = v;
  }
  return 1;

finally:
  Py_XDECREF(*message);
  Py_XDECREF(*filename);
  return 0;
}
/* end copied function! */

bool python_script_error_jump(
    const char *filepath, int *r_lineno, int *r_offset, int *r_lineno_end, int *r_offset_end)
{
  /* WARNING(@campbellbarton): The normalized exception is restored (losing line number info).
   * Ideally this would leave the exception state as it found it, but that needs to be done
   * carefully with regards to reference counting, see: T97731. */

  bool success = false;
  PyObject *exception, *value;
  PyTracebackObject *tb;

  *r_lineno = -1;
  *r_offset = 0;

  *r_lineno_end = -1;
  *r_offset_end = 0;

  PyErr_Fetch(&exception, &value, (PyObject **)&tb);
  if (exception == NULL) {
    return false;
  }

  if (PyErr_GivenExceptionMatches(exception, PyExc_SyntaxError)) {
    /* No trace-back available when `SyntaxError`.
     * Python has no API's to this. reference #parse_syntax_error() from `pythonrun.c`. */
    PyErr_NormalizeException(&exception, &value, (PyObject **)&tb);

    if (value) { /* Should always be true. */
      PyObject *message;
      PyObject *filepath_exc_py, *text_py;

      if (parse_syntax_error(value,
                             &message,
                             &filepath_exc_py,
                             r_lineno,
                             r_offset,
                             r_lineno_end,
                             r_offset_end,
                             &text_py)) {
        const char *filepath_exc = PyUnicode_AsUTF8(filepath_exc_py);
        /* python adds a '/', prefix, so check for both */
        if ((BLI_path_cmp(filepath_exc, filepath) == 0) ||
            (ELEM(filepath_exc[0], '\\', '/') && BLI_path_cmp(filepath_exc + 1, filepath) == 0)) {
          success = true;
        }
      }
    }
  }
  else {
    PyErr_NormalizeException(&exception, &value, (PyObject **)&tb);

    for (tb = (PyTracebackObject *)PySys_GetObject("last_traceback");
         tb && (PyObject *)tb != Py_None;
         tb = tb->tb_next) {
      PyObject *coerce;
      const char *tb_filepath = traceback_filepath(tb, &coerce);
      const int match = ((BLI_path_cmp(tb_filepath, filepath) == 0) ||
                         (ELEM(tb_filepath[0], '\\', '/') &&
                          BLI_path_cmp(tb_filepath + 1, filepath) == 0));
      Py_DECREF(coerce);

      if (match) {
        success = true;
        *r_lineno = *r_lineno_end = tb->tb_lineno;
        /* used to break here, but better find the inner most line */
      }
    }
  }

  PyErr_Restore(exception, value, (PyObject *)tb); /* takes away reference! */

  return success;
}
