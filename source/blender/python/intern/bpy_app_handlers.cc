/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup pythonintern
 *
 * This file defines a #PyStructSequence accessed via `bpy.app.handlers`,
 * which exposes various lists that the script author can add callback
 * functions into (called via blenders generic BLI_cb API)
 */

#include "BLI_utildefines.h"
#include <Python.h>

#include "../generic/python_compat.hh" /* IWYU pragma: keep. */

#include "BKE_callbacks.hh"

#include "RNA_access.hh"

#include "bpy_app_handlers.hh"
#include "bpy_rna.hh"

#include "BPY_extern.hh"

void bpy_app_generic_callback(Main *main,
                              PointerRNA **pointers,
                              const int pointers_num,
                              void *arg);

static PyTypeObject BlenderAppCbType;

#define FILEPATH_SAVE_ARG \
  "Accepts one argument: " \
  "the file being saved, an empty string for the startup-file."
#define FILEPATH_LOAD_ARG \
  "Accepts one argument: " \
  "the file being loaded, an empty string for the startup-file."
#define RENDER_STATS_ARG \
  "Accepts one argument: " \
  "the render stats (render/saving time plus in background mode frame/used [peak] memory)."
#define DEPSGRAPH_UPDATE_ARG \
  "Accepts two arguments: " \
  "The scene data-block and the dependency graph being updated"
#define RENDER_ARG \
  "Accepts one argument: " \
  "the scene data-block being rendered"
#define OBJECT_BAKE_ARG \
  "Accepts one argument: " \
  "the object data-block being baked"
#define COMPOSITE_ARG \
  "Accepts one argument: " \
  "the scene data-block"
#define ANNOTATION_ARG \
  "Accepts two arguments: " \
  "the annotation data-block and dependency graph"
#define BLENDIMPORT_ARG \
  "Accepts one argument: " \
  "a BlendImportContext"

/**
 * See `BKE_callbacks.hh` #eCbEvent declaration for the policy on naming.
 */
static PyStructSequence_Field app_cb_info_fields[] = {
    {"frame_change_pre",
     "Called after frame change for playback and rendering, before any data is evaluated for the "
     "new frame. This makes it possible to change data and relations (for example swap an object "
     "to another mesh) for the new frame. Note that this handler is **not** to be used as 'before "
     "the frame changes' event. The dependency graph is not available in this handler, as data "
     "and relations may have been altered and the dependency graph has not yet been updated for "
     "that. " DEPSGRAPH_UPDATE_ARG},
    {"frame_change_post",
     "Called after frame change for playback and rendering, after the data has been evaluated "
     "for the new frame. " DEPSGRAPH_UPDATE_ARG},
    {"render_pre", "on render (before)"},
    {"render_post", "on render (after)"},
    {"render_write", "on writing a render frame (directly after the frame is written)"},
    {"render_stats", "on printing render statistics. " RENDER_STATS_ARG},
    {"render_init", "on initialization of a render job. " RENDER_ARG},
    {"render_complete", "on completion of render job. " RENDER_ARG},
    {"render_cancel", "on canceling a render job. " RENDER_ARG},

    {"load_pre", "on loading a new blend file (before)." FILEPATH_LOAD_ARG},
    {"load_post", "on loading a new blend file (after). " FILEPATH_LOAD_ARG},
    {"load_post_fail", "on failure to load a new blend file (after). " FILEPATH_LOAD_ARG},

    {"save_pre", "on saving a blend file (before). " FILEPATH_SAVE_ARG},
    {"save_post", "on saving a blend file (after). " FILEPATH_SAVE_ARG},
    {"save_post_fail", "on failure to save a blend file (after). " FILEPATH_SAVE_ARG},

    {"undo_pre", "on loading an undo step (before)"},
    {"undo_post", "on loading an undo step (after)"},
    {"redo_pre", "on loading a redo step (before)"},
    {"redo_post", "on loading a redo step (after)"},
    {"depsgraph_update_pre", "on depsgraph update (pre). " DEPSGRAPH_UPDATE_ARG},
    {"depsgraph_update_post", "on depsgraph update (post). " DEPSGRAPH_UPDATE_ARG},
    {"game_pre", "on starting the game engine"},
    {"game_post", "on ending the game engine"},
    {"version_update", "on ending the versioning code"},
    {"load_factory_preferences_post", "on loading factory preferences (after)"},
    {"load_factory_startup_post", "on loading factory startup (after)"},
    {"xr_session_start_pre", "on starting an xr session (before)"},
    {"annotation_pre", "on drawing an annotation (before). " ANNOTATION_ARG},
    {"annotation_post", "on drawing an annotation (after). " ANNOTATION_ARG},
    {"object_bake_pre", "before starting a bake job. " OBJECT_BAKE_ARG},
    {"object_bake_complete",
     "on completing a bake job; will be called in the main thread. " OBJECT_BAKE_ARG},
    {"object_bake_cancel",
     "on canceling a bake job; will be called in the main thread. " OBJECT_BAKE_ARG},
    {"composite_pre", "on a compositing background job (before). " COMPOSITE_ARG},
    {"composite_post", "on a compositing background job (after). " COMPOSITE_ARG},
    {"composite_cancel", "on a compositing background job (cancel). " COMPOSITE_ARG},
    {"animation_playback_pre", "on starting animation playback. " DEPSGRAPH_UPDATE_ARG},
    {"animation_playback_post", "on ending animation playback. " DEPSGRAPH_UPDATE_ARG},
    {"translation_update_post", "on translation settings update"},
    /* NOTE(@ideasman42): This avoids bad-level calls into BPY API
     * but should not be considered part of the public Python API.
     * If there is a compelling reason to make these public, the leading `_` can be removed. */
    {"_extension_repos_update_pre", "on changes to extension repos (before)"},
    {"_extension_repos_update_post", "on changes to extension repos (after)"},
    {"_extension_repos_sync", "on creating or synchronizing the active repository"},
    {"_extension_repos_files_clear",
     "remove files from the repository directory (uses as a string argument)"},
    {"blend_import_pre", "on linking or appending data (before). " BLENDIMPORT_ARG},
    {"blend_import_post", "on linking or appending data (after). " BLENDIMPORT_ARG},

/* sets the permanent tag */
#define APP_CB_OTHER_FIELDS 1
    {"persistent",
     "Function decorator for callback functions not to be removed when loading new files"},

    {nullptr},
};

static PyStructSequence_Desc app_cb_info_desc = {
    /*name*/ "bpy.app.handlers",
    /*doc*/ "This module contains callback lists",
    /*fields*/ app_cb_info_fields,
    /*n_in_sequence*/ ARRAY_SIZE(app_cb_info_fields) - 1,
};

#if 0
#  if (BKE_CB_EVT_TOT != ARRAY_SIZE(app_cb_info_fields))
#    error "Callbacks are out of sync"
#  endif
#endif

/* -------------------------------------------------------------------- */
/** \name Permanent Tagging Code
 * \{ */

#define PERMINENT_CB_ID "_bpy_persistent"

static PyObject *bpy_app_handlers_persistent_new(PyTypeObject * /*type*/,
                                                 PyObject *args,
                                                 PyObject * /*kwds*/)
{
  PyObject *value;

  if (!PyArg_ParseTuple(args, "O:bpy.app.handlers.persistent", &value)) {
    return nullptr;
  }

  if (PyFunction_Check(value)) {
    PyObject **dict_ptr = _PyObject_GetDictPtr(value);
    if (dict_ptr == nullptr) {
      PyErr_SetString(PyExc_ValueError,
                      "bpy.app.handlers.persistent wasn't able to "
                      "get the dictionary from the function passed");
      return nullptr;
    }

    /* set id */
    if (*dict_ptr == nullptr) {
      *dict_ptr = PyDict_New();
    }

    PyDict_SetItemString(*dict_ptr, PERMINENT_CB_ID, Py_None);

    Py_INCREF(value);
    return value;
  }

  PyErr_SetString(PyExc_ValueError, "bpy.app.handlers.persistent expected a function");
  return nullptr;
}

/** Dummy type because decorators can't be a #PyCFunction. */
static PyTypeObject BPyPersistent_Type = {
#if defined(_MSC_VER)
    /*ob_base*/ PyVarObject_HEAD_INIT(nullptr, 0)
#else
    /*ob_base*/ PyVarObject_HEAD_INIT(&PyType_Type, 0)
#endif
    /*tp_name*/ "persistent",
    /*tp_basicsize*/ 0,
    /*tp_itemsize*/ 0,
    /*tp_dealloc*/ nullptr,
    /*tp_vectorcall_offset*/ 0,
    /*tp_getattr*/ nullptr,
    /*tp_setattr*/ nullptr,
    /*tp_as_async*/ nullptr,
    /*tp_repr*/ nullptr,
    /*tp_as_number*/ nullptr,
    /*tp_as_sequence*/ nullptr,
    /*tp_as_mapping*/ nullptr,
    /*tp_hash*/ nullptr,
    /*tp_call*/ nullptr,
    /*tp_str*/ nullptr,
    /*tp_getattro*/ nullptr,
    /*tp_setattro*/ nullptr,
    /*tp_as_buffer*/ nullptr,
    /*tp_flags*/ Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    /*tp_doc*/ nullptr,
    /*tp_traverse*/ nullptr,
    /*tp_clear*/ nullptr,
    /*tp_richcompare*/ nullptr,
    /*tp_weaklistoffset*/ 0,
    /*tp_iter*/ nullptr,
    /*tp_iternext*/ nullptr,
    /*tp_methods*/ nullptr,
    /*tp_members*/ nullptr,
    /*tp_getset*/ nullptr,
    /*tp_base*/ nullptr,
    /*tp_dict*/ nullptr,
    /*tp_descr_get*/ nullptr,
    /*tp_descr_set*/ nullptr,
    /*tp_dictoffset*/ 0,
    /*tp_init*/ nullptr,
    /*tp_alloc*/ nullptr,
    /*tp_new*/ bpy_app_handlers_persistent_new,
    /*tp_free*/ nullptr,
    /*tp_is_gc*/ nullptr,
    /*tp_bases*/ nullptr,
    /*tp_mro*/ nullptr,
    /*tp_cache*/ nullptr,
    /*tp_subclasses*/ nullptr,
    /*tp_weaklist*/ nullptr,
    /*tp_del*/ nullptr,
    /*tp_version_tag*/ 0,
    /*tp_finalize*/ nullptr,
    /*tp_vectorcall*/ nullptr,
};

/** \} */

static PyObject *py_cb_array[BKE_CB_EVT_TOT] = {nullptr};

static PyObject *make_app_cb_info()
{
  PyObject *app_cb_info;
  int pos;

  app_cb_info = PyStructSequence_New(&BlenderAppCbType);
  if (app_cb_info == nullptr) {
    return nullptr;
  }

  for (pos = 0; pos < BKE_CB_EVT_TOT; pos++) {
    if (app_cb_info_fields[pos].name == nullptr) {
      Py_FatalError("invalid callback slots 1");
    }
    PyStructSequence_SET_ITEM(app_cb_info, pos, (py_cb_array[pos] = PyList_New(0)));
  }
  if (app_cb_info_fields[pos + APP_CB_OTHER_FIELDS].name != nullptr) {
    Py_FatalError("invalid callback slots 2");
  }

  /* custom function */
  PyStructSequence_SET_ITEM(app_cb_info, pos++, (PyObject *)&BPyPersistent_Type);

  return app_cb_info;
}

PyObject *BPY_app_handlers_struct()
{
  PyObject *ret;

#if defined(_MSC_VER)
  BPyPersistent_Type.ob_base.ob_base.ob_type = &PyType_Type;
#endif

  if (PyType_Ready(&BPyPersistent_Type) < 0) {
    BLI_assert_msg(0, "error initializing 'bpy.app.handlers.persistent'");
  }

  PyStructSequence_InitType(&BlenderAppCbType, &app_cb_info_desc);

  ret = make_app_cb_info();

  /* prevent user from creating new instances */
  BlenderAppCbType.tp_init = nullptr;
  BlenderAppCbType.tp_new = nullptr;
  /* Without this we can't do `set(sys.modules)` #29635. */
  BlenderAppCbType.tp_hash = (hashfunc)Py_HashPointer;

  /* assign the C callbacks */
  if (ret) {
    static bCallbackFuncStore funcstore_array[BKE_CB_EVT_TOT] = {{nullptr}};
    bCallbackFuncStore *funcstore;
    int pos = 0;

    for (pos = 0; pos < BKE_CB_EVT_TOT; pos++) {
      funcstore = &funcstore_array[pos];
      funcstore->func = bpy_app_generic_callback;
      funcstore->alloc = 0;
      funcstore->arg = POINTER_FROM_INT(pos);
      BKE_callback_add(funcstore, eCbEvent(pos));
    }
  }

  return ret;
}

void BPY_app_handlers_reset(const bool do_all)
{
  PyGILState_STATE gilstate = PyGILState_Ensure();
  int pos = 0;

  if (do_all) {
    for (pos = 0; pos < BKE_CB_EVT_TOT; pos++) {
      /* clear list */
      PyList_SetSlice(py_cb_array[pos], 0, PY_SSIZE_T_MAX, nullptr);
    }
  }
  else {
    /* save string conversion thrashing */
    PyObject *perm_id_str = PyUnicode_FromString(PERMINENT_CB_ID);

    for (pos = 0; pos < BKE_CB_EVT_TOT; pos++) {
      /* clear only items without PERMINENT_CB_ID */
      PyObject *ls = py_cb_array[pos];
      Py_ssize_t i;

      for (i = PyList_GET_SIZE(ls) - 1; i >= 0; i--) {
        PyObject *item = PyList_GET_ITEM(ls, i);

        if (PyMethod_Check(item)) {
          PyObject *item_test = PyMethod_GET_FUNCTION(item);
          if (item_test) {
            item = item_test;
          }
        }

        PyObject **dict_ptr;
        if (PyFunction_Check(item) && (dict_ptr = _PyObject_GetDictPtr(item)) && (*dict_ptr) &&
            (PyDict_GetItem(*dict_ptr, perm_id_str) != nullptr))
        {
          /* keep */
        }
        else {
          /* remove */
          // PySequence_DelItem(ls, i); /* more obvious but slower */
          PyList_SetSlice(ls, i, i + 1, nullptr);
        }
      }
    }

    Py_DECREF(perm_id_str);
  }

  PyGILState_Release(gilstate);
}

static PyObject *choose_arguments(PyObject *func, PyObject *args_all, PyObject *args_single)
{
  if (!PyFunction_Check(func)) {
    return args_all;
  }
  PyCodeObject *code = (PyCodeObject *)PyFunction_GetCode(func);
  if (code->co_argcount == 1) {
    return args_single;
  }
  return args_all;
}

/* the actual callback - not necessarily called from py */
void bpy_app_generic_callback(Main * /*main*/,
                              PointerRNA **pointers,
                              const int pointers_num,
                              void *arg)
{
  PyObject *cb_list = py_cb_array[POINTER_AS_INT(arg)];
  if (PyList_GET_SIZE(cb_list) > 0) {
    const PyGILState_STATE gilstate = PyGILState_Ensure();

    const int num_arguments = 2;
    PyObject *args_all = PyTuple_New(num_arguments); /* save python creating each call */
    PyObject *args_single = PyTuple_New(1);
    PyObject *func;
    PyObject *ret;
    Py_ssize_t pos;

    /* setup arguments */
    for (int i = 0; i < pointers_num; ++i) {
      PyTuple_SET_ITEM(
          args_all, i, pyrna_struct_CreatePyObject_with_primitive_support(pointers[i]));
    }
    for (int i = pointers_num; i < num_arguments; ++i) {
      PyTuple_SET_ITEM(args_all, i, Py_NewRef(Py_None));
    }

    if (pointers_num == 0) {
      PyTuple_SET_ITEM(args_single, 0, Py_NewRef(Py_None));
    }
    else {
      PyTuple_SET_ITEM(
          args_single, 0, pyrna_struct_CreatePyObject_with_primitive_support(pointers[0]));
    }

    /* Iterate the list and run the callbacks
     * NOTE: don't store the list size since the scripts may remove themselves. */
    for (pos = 0; pos < PyList_GET_SIZE(cb_list); pos++) {
      func = PyList_GET_ITEM(cb_list, pos);
      PyObject *args = choose_arguments(func, args_all, args_single);
      ret = PyObject_Call(func, args, nullptr);
      if (ret == nullptr) {
        /* Don't set last system variables because they might cause some
         * dangling pointers to external render engines (when exception
         * happens during rendering) which will break logic of render pipeline
         * which expects to be the only user of render engine when rendering
         * is finished. */

        /* Note the handler called, the exception itself typically has the function name. */
        PySys_WriteStderr("Error in bpy.app.handlers.%s[%d]:\n",
                          app_cb_info_fields[POINTER_AS_INT(arg)].name,
                          int(pos));
        PyErr_PrintEx(0);
      }
      else {
        Py_DECREF(ret);
      }
    }

    Py_DECREF(args_all);
    Py_DECREF(args_single);

    PyGILState_Release(gilstate);
  }
}
