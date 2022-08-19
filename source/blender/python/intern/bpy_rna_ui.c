/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup pythonintern
 *
 * This adds helpers to #uiLayout which can't be added easily to RNA itself.
 */

#include <Python.h>

#include "MEM_guardedalloc.h"

#include "../generic/py_capi_utils.h"

#include "UI_interface.h"

#include "RNA_types.h"

#include "bpy_rna.h"
#include "bpy_rna_ui.h"

PyDoc_STRVAR(bpy_rna_uilayout_introspect_doc,
             ".. method:: introspect()\n"
             "\n"
             "   Return a dictionary containing a textual representation of the UI layout.\n");
static PyObject *bpy_rna_uilayout_introspect(PyObject *self)
{
  BPy_StructRNA *pyrna = (BPy_StructRNA *)self;
  uiLayout *layout = pyrna->ptr.data;

  const char *expr = UI_layout_introspect(layout);
  PyObject *main_mod = NULL;
  PyC_MainModule_Backup(&main_mod);
  PyObject *py_dict = PyC_DefaultNameSpace("<introspect>");
  PyObject *result = PyRun_String(expr, Py_eval_input, py_dict, py_dict);
  MEM_freeN((void *)expr);
  Py_DECREF(py_dict);
  PyC_MainModule_Restore(main_mod);
  return result;
}

PyMethodDef BPY_rna_uilayout_introspect_method_def = {
    "introspect",
    (PyCFunction)bpy_rna_uilayout_introspect,
    METH_NOARGS,
    bpy_rna_uilayout_introspect_doc,
};
