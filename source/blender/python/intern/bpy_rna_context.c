/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup pythonintern
 *
 * This file adds some helper methods to the context, that cannot fit well in RNA itself.
 */

#include <Python.h>

#include "BLI_listbase.h"
#include "BLI_utildefines.h"

#include "BKE_context.h"

#include "WM_api.h"
#include "WM_types.h"

#include "bpy_rna_context.h"

#include "RNA_access.h"
#include "RNA_prototypes.h"

#include "bpy_rna.h"

/* -------------------------------------------------------------------- */
/** \name Temporary Context Override (Python Context Manager)
 * \{ */

typedef struct ContextStore {
  wmWindow *win;
  bool win_is_set;
  ScrArea *area;
  bool area_is_set;
  ARegion *region;
  bool region_is_set;
} ContextStore;

typedef struct BPyContextTempOverride {
  PyObject_HEAD /* Required Python macro. */
  bContext *context;

  ContextStore ctx_init;
  ContextStore ctx_temp;
  /** Bypass Python overrides set when calling an operator from Python. */
  struct bContext_PyState py_state;
  /**
   * This dictionary is used to store members that don't have special handling,
   * see: #bpy_context_temp_override_extract_known_args,
   * these will then be accessed via #BPY_context_member_get.
   *
   * This also supports nested *stacking*, so a nested temp-context-overrides
   * will overlay the new members on the old members (instead of ignoring them).
   */
  PyObject *py_state_context_dict;
} BPyContextTempOverride;

static void bpy_rna_context_temp_override__tp_dealloc(BPyContextTempOverride *self)
{
  PyObject_DEL(self);
}

static PyObject *bpy_rna_context_temp_override_enter(BPyContextTempOverride *self)
{
  bContext *C = self->context;

  CTX_py_state_push(C, &self->py_state, self->py_state_context_dict);

  self->ctx_init.win = CTX_wm_window(C);
  self->ctx_init.win_is_set = (self->ctx_init.win != self->ctx_temp.win);
  self->ctx_init.area = CTX_wm_area(C);
  self->ctx_init.area_is_set = (self->ctx_init.area != self->ctx_temp.area);
  self->ctx_init.region = CTX_wm_region(C);
  self->ctx_init.region_is_set = (self->ctx_init.region != self->ctx_temp.region);

  wmWindow *win = self->ctx_temp.win_is_set ? self->ctx_temp.win : self->ctx_init.win;
  bScreen *screen = win ? WM_window_get_active_screen(win) : NULL;
  ScrArea *area = self->ctx_temp.area_is_set ? self->ctx_temp.area : self->ctx_init.area;
  ARegion *region = self->ctx_temp.region_is_set ? self->ctx_temp.region : self->ctx_init.region;

  /* Sanity check, the region is in the screen/area. */
  if (self->ctx_temp.region_is_set && (region != NULL)) {
    if (area == NULL) {
      PyErr_SetString(PyExc_TypeError, "Region set with NULL area");
      return NULL;
    }
    if ((screen && BLI_findindex(&screen->regionbase, region) == -1) &&
        (BLI_findindex(&area->regionbase, region) == -1)) {
      PyErr_SetString(PyExc_TypeError, "Region not found in area");
      return NULL;
    }
  }

  if (self->ctx_temp.area_is_set && (area != NULL)) {
    if (screen == NULL) {
      PyErr_SetString(PyExc_TypeError, "Area set with NULL screen");
      return NULL;
    }
    if (BLI_findindex(&screen->areabase, area) == -1) {
      PyErr_SetString(PyExc_TypeError, "Area not found in screen");
      return NULL;
    }
  }

  if (self->ctx_temp.win_is_set) {
    CTX_wm_window_set(C, self->ctx_temp.win);
  }
  if (self->ctx_temp.area_is_set) {
    CTX_wm_area_set(C, self->ctx_temp.area);
  }
  if (self->ctx_temp.region_is_set) {
    CTX_wm_region_set(C, self->ctx_temp.region);
  }

  Py_RETURN_NONE;
}

static PyObject *bpy_rna_context_temp_override_exit(BPyContextTempOverride *self,
                                                    PyObject *UNUSED(args))
{
  bContext *C = self->context;

  /* Special case where the window is expected to be freed on file-read,
   * in this case the window should not be restored, see: T92818. */
  bool do_restore = true;
  if (self->ctx_init.win) {
    wmWindowManager *wm = CTX_wm_manager(C);
    if (BLI_findindex(&wm->windows, self->ctx_init.win) == -1) {
      CTX_wm_window_set(C, NULL);
      do_restore = false;
    }
  }

  if (do_restore) {
    if (self->ctx_init.win_is_set) {
      CTX_wm_window_set(C, self->ctx_init.win);
    }
    if (self->ctx_init.area_is_set) {
      CTX_wm_area_set(C, self->ctx_init.area);
    }
    if (self->ctx_init.region_is_set) {
      CTX_wm_region_set(C, self->ctx_init.region);
    }
  }

  /* A copy may have been made when writing context members, see #BPY_context_dict_clear_members */
  PyObject *context_dict_test = CTX_py_dict_get(C);
  if (context_dict_test && (context_dict_test != self->py_state_context_dict)) {
    Py_DECREF(context_dict_test);
  }
  CTX_py_state_pop(C, &self->py_state);
  Py_CLEAR(self->py_state_context_dict);

  Py_RETURN_NONE;
}

static PyMethodDef bpy_rna_context_temp_override__tp_methods[] = {
    {"__enter__", (PyCFunction)bpy_rna_context_temp_override_enter, METH_NOARGS},
    {"__exit__", (PyCFunction)bpy_rna_context_temp_override_exit, METH_VARARGS},
    {NULL},
};

static PyTypeObject BPyContextTempOverride_Type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "ContextTempOverride",
    .tp_basicsize = sizeof(BPyContextTempOverride),
    .tp_dealloc = (destructor)bpy_rna_context_temp_override__tp_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_methods = bpy_rna_context_temp_override__tp_methods,
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Context Temporary Override Method
 * \{ */

static PyObject *bpy_context_temp_override_extract_known_args(const char *const *kwds_static,
                                                              PyObject *kwds)
{
  PyObject *sentinel = Py_Ellipsis;
  PyObject *kwds_parse = PyDict_New();
  for (int i = 0; kwds_static[i]; i++) {
    PyObject *key = PyUnicode_FromString(kwds_static[i]);
    PyObject *val = _PyDict_Pop(kwds, key, sentinel);
    if (val != sentinel) {
      if (PyDict_SetItem(kwds_parse, key, val) == -1) {
        BLI_assert_unreachable();
      }
    }
    Py_DECREF(key);
    Py_DECREF(val);
  }
  return kwds_parse;
}

PyDoc_STRVAR(bpy_context_temp_override_doc,
             ".. method:: temp_override(window, area, region, **keywords)\n"
             "\n"
             "   Context manager to temporarily override members in the context.\n"
             "\n"
             "   :arg window: Window override or None.\n"
             "   :type window: :class:`bpy.types.Window`\n"
             "   :arg area: Area override or None.\n"
             "   :type area: :class:`bpy.types.Area`\n"
             "   :arg region: Region override or None.\n"
             "   :type region: :class:`bpy.types.Region`\n"
             "   :arg keywords: Additional keywords override context members.\n"
             "   :return: The context manager .\n"
             "   :rtype: context manager\n");
static PyObject *bpy_context_temp_override(PyObject *self, PyObject *args, PyObject *kwds)
{
  const PointerRNA *context_ptr = pyrna_struct_as_ptr(self, &RNA_Context);
  if (context_ptr == NULL) {
    return NULL;
  }

  if (kwds == NULL) {
    /* While this is effectively NOP, support having no keywords as it's more involved
     * to return an alternative (dummy) context manager. */
  }
  else {
    /* Needed because the keywords copied into `kwds_parse` could contain anything.
     * As the types of keys aren't checked. */
    if (!PyArg_ValidateKeywordArguments(kwds)) {
      return NULL;
    }
  }

  struct {
    struct BPy_StructRNA_Parse window;
    struct BPy_StructRNA_Parse area;
    struct BPy_StructRNA_Parse region;
  } params = {
      .window = {.type = &RNA_Window},
      .area = {.type = &RNA_Area},
      .region = {.type = &RNA_Region},
  };

  static const char *const _keywords[] = {"window", "area", "region", NULL};
  static _PyArg_Parser _parser = {
      "|$" /* Optional, keyword only arguments. */
      "O&" /* `window` */
      "O&" /* `area` */
      "O&" /* `region` */
      ":temp_override",
      _keywords,
      0,
  };
  /* Parse known keywords, the remaining keywords are set using #CTX_py_state_push. */
  kwds = kwds ? PyDict_Copy(kwds) : PyDict_New();
  {
    PyObject *kwds_parse = bpy_context_temp_override_extract_known_args(_keywords, kwds);
    const int parse_result = _PyArg_ParseTupleAndKeywordsFast(args,
                                                              kwds_parse,
                                                              &_parser,
                                                              pyrna_struct_as_ptr_or_null_parse,
                                                              &params.window,
                                                              pyrna_struct_as_ptr_or_null_parse,
                                                              &params.area,
                                                              pyrna_struct_as_ptr_or_null_parse,
                                                              &params.region);
    Py_DECREF(kwds_parse);
    if (parse_result == -1) {
      Py_DECREF(kwds);
      return NULL;
    }
  }

  bContext *C = context_ptr->data;
  {
    /* Merge existing keys that don't exist in the keywords passed in.
     * This makes it possible to nest context overrides. */
    PyObject *context_dict_current = CTX_py_dict_get(C);
    if (context_dict_current != NULL) {
      PyDict_Merge(kwds, context_dict_current, 0);
    }
  }

  ContextStore ctx_temp = {NULL};
  if (params.window.ptr != NULL) {
    ctx_temp.win = params.window.ptr->data;
    ctx_temp.win_is_set = true;
  }
  if (params.area.ptr != NULL) {
    ctx_temp.area = params.area.ptr->data;
    ctx_temp.area_is_set = true;
  }

  if (params.region.ptr != NULL) {
    ctx_temp.region = params.region.ptr->data;
    ctx_temp.region_is_set = true;
  }

  BPyContextTempOverride *ret = PyObject_New(BPyContextTempOverride, &BPyContextTempOverride_Type);
  ret->context = C;
  ret->ctx_temp = ctx_temp;
  memset(&ret->ctx_init, 0, sizeof(ret->ctx_init));

  ret->py_state_context_dict = kwds;

  return (PyObject *)ret;
}

/** \} */

PyMethodDef BPY_rna_context_temp_override_method_def = {
    "temp_override",
    (PyCFunction)bpy_context_temp_override,
    METH_VARARGS | METH_KEYWORDS,
    bpy_context_temp_override_doc,
};

void bpy_rna_context_types_init(void)
{
  if (PyType_Ready(&BPyContextTempOverride_Type) < 0) {
    BLI_assert_unreachable();
    return;
  }
}
