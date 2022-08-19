/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup pythonintern
 *
 * This file defines the '_bpy' module which is used by python's 'bpy' package
 * to access C defined builtin functions.
 * A script writer should never directly access this module.
 */

/* Future-proof, See https://docs.python.org/3/c-api/arg.html#strings-and-buffers */
#define PY_SSIZE_T_CLEAN

#include <Python.h>

#include "BLI_string.h"
#include "BLI_string_utils.h"
#include "BLI_utildefines.h"

#include "BKE_appdir.h"
#include "BKE_blender_version.h"
#include "BKE_bpath.h"
#include "BKE_global.h" /* XXX, G_MAIN only */

#include "RNA_access.h"
#include "RNA_enum_types.h"
#include "RNA_prototypes.h"
#include "RNA_types.h"

#include "GPU_state.h"

#include "bpy.h"
#include "bpy_app.h"
#include "bpy_capi_utils.h"
#include "bpy_driver.h"
#include "bpy_library.h"
#include "bpy_operator.h"
#include "bpy_props.h"
#include "bpy_rna.h"
#include "bpy_rna_data.h"
#include "bpy_rna_gizmo.h"
#include "bpy_rna_id_collection.h"
#include "bpy_rna_types_capi.h"
#include "bpy_utils_previews.h"
#include "bpy_utils_units.h"

#include "../generic/py_capi_utils.h"
#include "../generic/python_utildefines.h"

/* external util modules */
#include "../generic/idprop_py_api.h"
#include "../generic/idprop_py_ui_api.h"
#include "bpy_msgbus.h"

#ifdef WITH_FREESTYLE
#  include "BPy_Freestyle.h"
#endif

PyObject *bpy_package_py = NULL;
PyObject *bpy_sys_module_backup = NULL;

PyDoc_STRVAR(bpy_script_paths_doc,
             ".. function:: script_paths()\n"
             "\n"
             "   Return 2 paths to blender scripts directories.\n"
             "\n"
             "   :return: (system, user) strings will be empty when not found.\n"
             "   :rtype: tuple of strings\n");
static PyObject *bpy_script_paths(PyObject *UNUSED(self))
{
  PyObject *ret = PyTuple_New(2);
  PyObject *item;
  const char *path;

  path = BKE_appdir_folder_id(BLENDER_SYSTEM_SCRIPTS, NULL);
  item = PyC_UnicodeFromByte(path ? path : "");
  BLI_assert(item != NULL);
  PyTuple_SET_ITEM(ret, 0, item);
  path = BKE_appdir_folder_id(BLENDER_USER_SCRIPTS, NULL);
  item = PyC_UnicodeFromByte(path ? path : "");
  BLI_assert(item != NULL);
  PyTuple_SET_ITEM(ret, 1, item);

  return ret;
}

static bool bpy_blend_foreach_path_cb(BPathForeachPathData *bpath_data,
                                      char *UNUSED(path_dst),
                                      const char *path_src)
{
  PyObject *py_list = bpath_data->user_data;
  PyList_APPEND(py_list, PyC_UnicodeFromByte(path_src));
  return false; /* Never edits the path. */
}

PyDoc_STRVAR(bpy_blend_paths_doc,
             ".. function:: blend_paths(absolute=False, packed=False, local=False)\n"
             "\n"
             "   Returns a list of paths to external files referenced by the loaded .blend file.\n"
             "\n"
             "   :arg absolute: When true the paths returned are made absolute.\n"
             "   :type absolute: boolean\n"
             "   :arg packed: When true skip file paths for packed data.\n"
             "   :type packed: boolean\n"
             "   :arg local: When true skip linked library paths.\n"
             "   :type local: boolean\n"
             "   :return: path list.\n"
             "   :rtype: list of strings\n");
static PyObject *bpy_blend_paths(PyObject *UNUSED(self), PyObject *args, PyObject *kw)
{
  eBPathForeachFlag flag = 0;
  PyObject *list;

  bool absolute = false;
  bool packed = false;
  bool local = false;

  static const char *_keywords[] = {"absolute", "packed", "local", NULL};
  static _PyArg_Parser _parser = {
      "|$" /* Optional keyword only arguments. */
      "O&" /* `absolute` */
      "O&" /* `packed` */
      "O&" /* `local` */
      ":blend_paths",
      _keywords,
      0,
  };
  if (!_PyArg_ParseTupleAndKeywordsFast(args,
                                        kw,
                                        &_parser,
                                        PyC_ParseBool,
                                        &absolute,
                                        PyC_ParseBool,
                                        &packed,
                                        PyC_ParseBool,
                                        &local)) {
    return NULL;
  }

  if (absolute) {
    flag |= BKE_BPATH_FOREACH_PATH_ABSOLUTE;
  }
  if (!packed) {
    flag |= BKE_BPATH_FOREACH_PATH_SKIP_PACKED;
  }
  if (local) {
    flag |= BKE_BPATH_FOREACH_PATH_SKIP_LINKED;
  }

  list = PyList_New(0);

  BKE_bpath_foreach_path_main(&(BPathForeachPathData){
      .bmain = G_MAIN,
      .callback_function = bpy_blend_foreach_path_cb,
      .flag = flag,
      .user_data = list,
  });

  return list;
}

PyDoc_STRVAR(bpy_flip_name_doc,
             ".. function:: flip_name(name, strip_digits=False)\n"
             "\n"
             "   Flip a name between left/right sides, useful for \n"
             "   mirroring bone names.\n"
             "\n"
             "   :arg name: Bone name to flip.\n"
             "   :type name: string\n"
             "   :arg strip_digits: Whether to remove ``.###`` suffix.\n"
             "   :type strip_digits: bool\n"
             "   :return: The flipped name.\n"
             "   :rtype: string\n");
static PyObject *bpy_flip_name(PyObject *UNUSED(self), PyObject *args, PyObject *kw)
{
  const char *name_src = NULL;
  Py_ssize_t name_src_len;
  bool strip_digits = false;

  static const char *_keywords[] = {"", "strip_digits", NULL};
  static _PyArg_Parser _parser = {
      "s#" /* `name` */
      "|$" /* Optional, keyword only arguments. */
      "O&" /* `strip_digits` */
      ":flip_name",
      _keywords,
      0,
  };
  if (!_PyArg_ParseTupleAndKeywordsFast(
          args, kw, &_parser, &name_src, &name_src_len, PyC_ParseBool, &strip_digits)) {
    return NULL;
  }

  /* Worst case we gain one extra byte (besides null-terminator) by changing
   * "Left" to "Right", because only the first appearance of "Left" gets replaced. */
  const size_t size = name_src_len + 2;
  char *name_dst = PyMem_MALLOC(size);
  const size_t name_dst_len = BLI_string_flip_side_name(name_dst, name_src, strip_digits, size);

  PyObject *result = PyUnicode_FromStringAndSize(name_dst, name_dst_len);

  PyMem_FREE(name_dst);

  return result;
}

// PyDoc_STRVAR(bpy_user_resource_doc[] = /* now in bpy/utils.py */
static PyObject *bpy_user_resource(PyObject *UNUSED(self), PyObject *args, PyObject *kw)
{
  const struct PyC_StringEnumItems type_items[] = {
      {BLENDER_USER_DATAFILES, "DATAFILES"},
      {BLENDER_USER_CONFIG, "CONFIG"},
      {BLENDER_USER_SCRIPTS, "SCRIPTS"},
      {BLENDER_USER_AUTOSAVE, "AUTOSAVE"},
      {0, NULL},
  };
  struct PyC_StringEnum type = {type_items};

  const char *subdir = NULL;

  const char *path;

  static const char *_keywords[] = {"type", "path", NULL};
  static _PyArg_Parser _parser = {
      "O&" /* `type` */
      "|$" /* Optional keyword only arguments. */
      "s"  /* `path` */
      ":user_resource",
      _keywords,
      0,
  };
  if (!_PyArg_ParseTupleAndKeywordsFast(args, kw, &_parser, PyC_ParseStringEnum, &type, &subdir)) {
    return NULL;
  }

  /* same logic as BKE_appdir_folder_id_create(),
   * but best leave it up to the script author to create */
  path = BKE_appdir_folder_id_user_notest(type.value_found, subdir);

  return PyC_UnicodeFromByte(path ? path : "");
}

PyDoc_STRVAR(bpy_system_resource_doc,
             ".. function:: system_resource(type, path=\"\")\n"
             "\n"
             "   Return a system resource path.\n"
             "\n"
             "   :arg type: string in ['DATAFILES', 'SCRIPTS', 'PYTHON'].\n"
             "   :type type: string\n"
             "   :arg path: Optional subdirectory.\n"
             "   :type path: string\n");
static PyObject *bpy_system_resource(PyObject *UNUSED(self), PyObject *args, PyObject *kw)
{
  const struct PyC_StringEnumItems type_items[] = {
      {BLENDER_SYSTEM_DATAFILES, "DATAFILES"},
      {BLENDER_SYSTEM_SCRIPTS, "SCRIPTS"},
      {BLENDER_SYSTEM_PYTHON, "PYTHON"},
      {0, NULL},
  };
  struct PyC_StringEnum type = {type_items};

  const char *subdir = NULL;

  const char *path;

  static const char *_keywords[] = {"type", "path", NULL};
  static _PyArg_Parser _parser = {
      "O&" /* `type` */
      "|$" /* Optional keyword only arguments. */
      "s"  /* `path` */
      ":system_resource",
      _keywords,
      0,
  };
  if (!_PyArg_ParseTupleAndKeywordsFast(args, kw, &_parser, PyC_ParseStringEnum, &type, &subdir)) {
    return NULL;
  }

  path = BKE_appdir_folder_id(type.value_found, subdir);

  return PyC_UnicodeFromByte(path ? path : "");
}

PyDoc_STRVAR(
    bpy_resource_path_doc,
    ".. function:: resource_path(type, major=bpy.app.version[0], minor=bpy.app.version[1])\n"
    "\n"
    "   Return the base path for storing system files.\n"
    "\n"
    "   :arg type: string in ['USER', 'LOCAL', 'SYSTEM'].\n"
    "   :type type: string\n"
    "   :arg major: major version, defaults to current.\n"
    "   :type major: int\n"
    "   :arg minor: minor version, defaults to current.\n"
    "   :type minor: string\n"
    "   :return: the resource path (not necessarily existing).\n"
    "   :rtype: string\n");
static PyObject *bpy_resource_path(PyObject *UNUSED(self), PyObject *args, PyObject *kw)
{
  const struct PyC_StringEnumItems type_items[] = {
      {BLENDER_RESOURCE_PATH_USER, "USER"},
      {BLENDER_RESOURCE_PATH_LOCAL, "LOCAL"},
      {BLENDER_RESOURCE_PATH_SYSTEM, "SYSTEM"},
      {0, NULL},
  };
  struct PyC_StringEnum type = {type_items};

  int major = BLENDER_VERSION / 100, minor = BLENDER_VERSION % 100;
  const char *path;

  static const char *_keywords[] = {"type", "major", "minor", NULL};
  static _PyArg_Parser _parser = {
      "O&" /* `type` */
      "|$" /* Optional keyword only arguments. */
      "i"  /* `major` */
      "i"  /* `minor` */
      ":resource_path",
      _keywords,
      0,
  };
  if (!_PyArg_ParseTupleAndKeywordsFast(
          args, kw, &_parser, PyC_ParseStringEnum, &type, &major, &minor)) {
    return NULL;
  }

  path = BKE_appdir_folder_id_version(type.value_found, (major * 100) + minor, false);

  return PyC_UnicodeFromByte(path ? path : "");
}

/* This is only exposed for tests, see: `tests/python/bl_pyapi_bpy_driver_secure_eval.py`. */
PyDoc_STRVAR(bpy_driver_secure_code_test_doc,
             ".. function:: _driver_secure_code_test(code)\n"
             "\n"
             "   Test if the script should be considered trusted.\n"
             "\n"
             "   :arg code: The code to test.\n"
             "   :type code: code\n"
             "   :arg namespace: The namespace of values which are allowed.\n"
             "   :type namespace: dict\n"
             "   :arg verbose: Print the reason for considering insecure to the ``stderr``.\n"
             "   :type verbose: bool\n"
             "   :return: True when the script is considered trusted.\n"
             "   :rtype: bool\n");
static PyObject *bpy_driver_secure_code_test(PyObject *UNUSED(self), PyObject *args, PyObject *kw)
{
  PyObject *py_code;
  PyObject *py_namespace = NULL;
  const bool verbose = false;
  static const char *_keywords[] = {"code", "namespace", "verbose", NULL};
  static _PyArg_Parser _parser = {
      "O!" /* `expression` */
      "|$" /* Optional keyword only arguments. */
      "O!" /* `namespace` */
      "O&" /* `verbose` */
      ":driver_secure_code_test",
      _keywords,
      0,
  };
  if (!_PyArg_ParseTupleAndKeywordsFast(args,
                                        kw,
                                        &_parser,
                                        &PyCode_Type,
                                        &py_code,
                                        &PyDict_Type,
                                        &py_namespace,
                                        PyC_ParseBool,
                                        &verbose)) {
    return NULL;
  }
  return PyBool_FromLong(BPY_driver_secure_bytecode_test(py_code, py_namespace, verbose));
}

PyDoc_STRVAR(bpy_escape_identifier_doc,
             ".. function:: escape_identifier(string)\n"
             "\n"
             "   Simple string escaping function used for animation paths.\n"
             "\n"
             "   :arg string: text\n"
             "   :type string: string\n"
             "   :return: The escaped string.\n"
             "   :rtype: string\n");
static PyObject *bpy_escape_identifier(PyObject *UNUSED(self), PyObject *value)
{
  Py_ssize_t value_str_len;
  const char *value_str = PyUnicode_AsUTF8AndSize(value, &value_str_len);

  if (value_str == NULL) {
    PyErr_SetString(PyExc_TypeError, "expected a string");
    return NULL;
  }

  const size_t size = (value_str_len * 2) + 1;
  char *value_escape_str = PyMem_MALLOC(size);
  const Py_ssize_t value_escape_str_len = BLI_str_escape(value_escape_str, value_str, size);

  PyObject *value_escape;
  if (value_escape_str_len == value_str_len) {
    Py_INCREF(value);
    value_escape = value;
  }
  else {
    value_escape = PyUnicode_FromStringAndSize(value_escape_str, value_escape_str_len);
  }

  PyMem_FREE(value_escape_str);

  return value_escape;
}

PyDoc_STRVAR(bpy_unescape_identifier_doc,
             ".. function:: unescape_identifier(string)\n"
             "\n"
             "   Simple string un-escape function used for animation paths.\n"
             "   This performs the reverse of `escape_identifier`.\n"
             "\n"
             "   :arg string: text\n"
             "   :type string: string\n"
             "   :return: The un-escaped string.\n"
             "   :rtype: string\n");
static PyObject *bpy_unescape_identifier(PyObject *UNUSED(self), PyObject *value)
{
  Py_ssize_t value_str_len;
  const char *value_str = PyUnicode_AsUTF8AndSize(value, &value_str_len);

  if (value_str == NULL) {
    PyErr_SetString(PyExc_TypeError, "expected a string");
    return NULL;
  }

  const size_t size = value_str_len + 1;
  char *value_unescape_str = PyMem_MALLOC(size);
  const Py_ssize_t value_unescape_str_len = BLI_str_unescape(value_unescape_str, value_str, size);

  PyObject *value_unescape;
  if (value_unescape_str_len == value_str_len) {
    Py_INCREF(value);
    value_unescape = value;
  }
  else {
    value_unescape = PyUnicode_FromStringAndSize(value_unescape_str, value_unescape_str_len);
  }

  PyMem_FREE(value_unescape_str);

  return value_unescape;
}

/**
 * \note only exposed for generating documentation, see: `doc/python_api/sphinx_doc_gen.py`.
 */
PyDoc_STRVAR(
    bpy_context_members_doc,
    ".. function:: context_members()\n"
    "\n"
    "   :return: A dict where the key is the context and the value is a tuple of it's members.\n"
    "   :rtype: dict\n");
static PyObject *bpy_context_members(PyObject *UNUSED(self))
{
  extern const char *buttons_context_dir[];
  extern const char *clip_context_dir[];
  extern const char *file_context_dir[];
  extern const char *image_context_dir[];
  extern const char *node_context_dir[];
  extern const char *screen_context_dir[];
  extern const char *sequencer_context_dir[];
  extern const char *text_context_dir[];
  extern const char *view3d_context_dir[];

  struct {
    const char *name;
    const char **dir;
  } context_members_all[] = {
      {"buttons", buttons_context_dir},
      {"clip", clip_context_dir},
      {"file", file_context_dir},
      {"image", image_context_dir},
      {"node", node_context_dir},
      {"screen", screen_context_dir},
      {"sequencer", sequencer_context_dir},
      {"text", text_context_dir},
      {"view3d", view3d_context_dir},
  };

  PyObject *result = _PyDict_NewPresized(ARRAY_SIZE(context_members_all));
  for (int context_index = 0; context_index < ARRAY_SIZE(context_members_all); context_index++) {
    const char *name = context_members_all[context_index].name;
    const char **dir = context_members_all[context_index].dir;
    int i;
    for (i = 0; dir[i]; i++) {
      /* Pass. */
    }
    PyObject *members = PyTuple_New(i);
    for (i = 0; dir[i]; i++) {
      PyTuple_SET_ITEM(members, i, PyUnicode_FromString(dir[i]));
    }
    PyDict_SetItemString(result, name, members);
    Py_DECREF(members);
  }
  BLI_assert(PyDict_GET_SIZE(result) == ARRAY_SIZE(context_members_all));

  return result;
}

/**
 * \note only exposed for generating documentation, see: `doc/python_api/sphinx_doc_gen.py`.
 */
PyDoc_STRVAR(bpy_rna_enum_items_static_doc,
             ".. function:: rna_enum_items_static()\n"
             "\n"
             "   :return: A dict where the key the name of the enum, the value is a tuple of "
             ":class:`bpy.types.EnumPropertyItem`.\n"
             "   :rtype: dict of \n");
static PyObject *bpy_rna_enum_items_static(PyObject *UNUSED(self))
{
#define DEF_ENUM(id) {STRINGIFY(id), id},
  struct {
    const char *id;
    const EnumPropertyItem *items;
  } enum_info[] = {
#include "RNA_enum_items.h"
  };
  PyObject *result = _PyDict_NewPresized(ARRAY_SIZE(enum_info));
  for (int i = 0; i < ARRAY_SIZE(enum_info); i++) {
    /* Include all items (including headings & separators), can be shown in documentation. */
    const EnumPropertyItem *items = enum_info[i].items;
    const int items_count = RNA_enum_items_count(items);
    PyObject *value = PyTuple_New(items_count);
    for (int item_index = 0; item_index < items_count; item_index++) {
      PointerRNA ptr;
      RNA_pointer_create(NULL, &RNA_EnumPropertyItem, (void *)&items[item_index], &ptr);
      PyTuple_SET_ITEM(value, item_index, pyrna_struct_CreatePyObject(&ptr));
    }
    PyDict_SetItemString(result, enum_info[i].id, value);
    Py_DECREF(value);
  }
  return result;
}

static PyMethodDef bpy_methods[] = {
    {"script_paths", (PyCFunction)bpy_script_paths, METH_NOARGS, bpy_script_paths_doc},
    {"blend_paths",
     (PyCFunction)bpy_blend_paths,
     METH_VARARGS | METH_KEYWORDS,
     bpy_blend_paths_doc},
    {"flip_name", (PyCFunction)bpy_flip_name, METH_VARARGS | METH_KEYWORDS, bpy_flip_name_doc},
    {"user_resource", (PyCFunction)bpy_user_resource, METH_VARARGS | METH_KEYWORDS, NULL},
    {"system_resource",
     (PyCFunction)bpy_system_resource,
     METH_VARARGS | METH_KEYWORDS,
     bpy_system_resource_doc},
    {"resource_path",
     (PyCFunction)bpy_resource_path,
     METH_VARARGS | METH_KEYWORDS,
     bpy_resource_path_doc},
    {"_driver_secure_code_test",
     (PyCFunction)bpy_driver_secure_code_test,
     METH_VARARGS | METH_KEYWORDS,
     bpy_driver_secure_code_test_doc},
    {"escape_identifier", (PyCFunction)bpy_escape_identifier, METH_O, bpy_escape_identifier_doc},
    {"unescape_identifier",
     (PyCFunction)bpy_unescape_identifier,
     METH_O,
     bpy_unescape_identifier_doc},
    {"context_members", (PyCFunction)bpy_context_members, METH_NOARGS, bpy_context_members_doc},
    {"rna_enum_items_static",
     (PyCFunction)bpy_rna_enum_items_static,
     METH_NOARGS,
     bpy_rna_enum_items_static_doc},
    {NULL, NULL, 0, NULL},
};

static PyObject *bpy_import_test(const char *modname)
{
  PyObject *mod = PyImport_ImportModuleLevel(modname, NULL, NULL, NULL, 0);

  GPU_bgl_end();

  if (mod) {
    Py_DECREF(mod);
  }
  else {
    PyErr_Print();
    PyErr_Clear();
  }

  return mod;
}

void BPy_init_modules(struct bContext *C)
{
  PointerRNA ctx_ptr;
  PyObject *mod;

  /* Needs to be first since this dir is needed for future modules */
  const char *const modpath = BKE_appdir_folder_id(BLENDER_SYSTEM_SCRIPTS, "modules");
  if (modpath) {
    // printf("bpy: found module path '%s'.\n", modpath);
    PyObject *sys_path = PySys_GetObject("path"); /* borrow */
    PyObject *py_modpath = PyUnicode_FromString(modpath);
    PyList_Insert(sys_path, 0, py_modpath); /* add first */
    Py_DECREF(py_modpath);
  }
  else {
    printf("bpy: couldn't find 'scripts/modules', blender probably won't start.\n");
  }
  /* stand alone utility modules not related to blender directly */
  IDProp_Init_Types(); /* not actually a submodule, just types */
  IDPropertyUIData_Init_Types();
#ifdef WITH_FREESTYLE
  Freestyle_Init();
#endif

  mod = PyModule_New("_bpy");

  /* add the module so we can import it */
  PyDict_SetItemString(PyImport_GetModuleDict(), "_bpy", mod);
  Py_DECREF(mod);

  /* needs to be first so bpy_types can run */
  PyModule_AddObject(mod, "types", BPY_rna_types());

  /* needs to be first so bpy_types can run */
  BPY_library_load_type_ready();

  BPY_rna_data_context_type_ready();

  BPY_rna_gizmo_module(mod);

  bpy_import_test("bpy_types");
  PyModule_AddObject(mod, "data", BPY_rna_module()); /* imports bpy_types by running this */
  bpy_import_test("bpy_types");
  PyModule_AddObject(mod, "props", BPY_rna_props());
  /* ops is now a python module that does the conversion from SOME_OT_foo -> some.foo */
  PyModule_AddObject(mod, "ops", BPY_operator_module());
  PyModule_AddObject(mod, "app", BPY_app_struct());
  PyModule_AddObject(mod, "_utils_units", BPY_utils_units());
  PyModule_AddObject(mod, "_utils_previews", BPY_utils_previews_module());
  PyModule_AddObject(mod, "msgbus", BPY_msgbus_module());

  RNA_pointer_create(NULL, &RNA_Context, C, &ctx_ptr);
  bpy_context_module = (BPy_StructRNA *)pyrna_struct_CreatePyObject(&ctx_ptr);
  /* odd that this is needed, 1 ref on creation and another for the module
   * but without we get a crash on exit */
  Py_INCREF(bpy_context_module);

  PyModule_AddObject(mod, "context", (PyObject *)bpy_context_module);

  /* Register methods and property get/set for RNA types. */
  BPY_rna_types_extend_capi();

  for (int i = 0; bpy_methods[i].ml_name; i++) {
    PyMethodDef *m = &bpy_methods[i];
    /* Currently there is no need to support these. */
    BLI_assert((m->ml_flags & (METH_CLASS | METH_STATIC)) == 0);
    PyModule_AddObject(mod, m->ml_name, (PyObject *)PyCFunction_New(m, NULL));
  }

  /* register funcs (bpy_rna.c) */
  PyModule_AddObject(mod,
                     meth_bpy_register_class.ml_name,
                     (PyObject *)PyCFunction_New(&meth_bpy_register_class, NULL));
  PyModule_AddObject(mod,
                     meth_bpy_unregister_class.ml_name,
                     (PyObject *)PyCFunction_New(&meth_bpy_unregister_class, NULL));

  PyModule_AddObject(mod,
                     meth_bpy_owner_id_get.ml_name,
                     (PyObject *)PyCFunction_New(&meth_bpy_owner_id_get, NULL));
  PyModule_AddObject(mod,
                     meth_bpy_owner_id_set.ml_name,
                     (PyObject *)PyCFunction_New(&meth_bpy_owner_id_set, NULL));

  /* add our own modules dir, this is a python package */
  bpy_package_py = bpy_import_test("bpy");
  bpy_sys_module_backup = PyDict_Copy(PyImport_GetModuleDict());
}

void BPy_end_modules(void)
{
  Py_DECREF(bpy_sys_module_backup);
}
