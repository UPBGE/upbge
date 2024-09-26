/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

/** \file
 * \ingroup pygen
 *
 * This file defines replacements for pythons '__import__' and 'imp.reload'
 * functions which can import from blender textblocks.
 *
 * \note
 * This should eventually be replaced by import hooks (pep 302).
 */

#include <Python.h>
#include <stddef.h>

#include "MEM_guardedalloc.h"

#include "DNA_text_types.h"

#include "BLI_listbase.h"
#include "BLI_path_utils.hh"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "BKE_main.hh"
/* UNUSED */
#include "BKE_text.h" /* txt_to_buf */

#include "py_capi_utils.hh"
#include "python_compat.hh"

#include "bpy_internal_import.hh" /* own include */

static Main *bpy_import_main = nullptr;
static ListBase bpy_import_main_list;

/* 'builtins' is most likely PyEval_GetBuiltins() */

/**
 * \note to the discerning developer, yes - this is nasty
 * monkey-patching our own import into Python's builtin 'imp' module.
 *
 * However Python's alternative is to use import hooks,
 * which are implemented in a way that we can't use our own importer as a
 * fall-back (instead we must try and fail - raise an exception every time).
 * Since importing from blenders text-blocks is not the common case
 * I prefer to use Pythons import by default and fall-back to
 * Blenders - which we can only do by intercepting import calls I'm afraid.
 * - Campbell
 */
void bpy_import_init(PyObject *builtins)
{
  PyObject *item;

  PyDict_SetItemString(builtins, "__import__", item = PyCFunction_New(&bpy_import_meth, nullptr));
  Py_DECREF(item);
}

static void free_compiled_text(Text *text)
{
  if (text->compiled) {
    Py_DECREF((PyObject *)text->compiled);
  }
  text->compiled = nullptr;
}

struct Main *bpy_import_main_get(void)
{
  return bpy_import_main;
}

void bpy_import_main_set(struct Main *maggie)
{
  bpy_import_main = maggie;
}

void bpy_import_main_extra_add(struct Main *maggie)
{
  BLI_addhead(&bpy_import_main_list, maggie);
}

void bpy_import_main_extra_remove(struct Main *maggie)
{
  BLI_remlink_safe(&bpy_import_main_list, maggie);
}

/* returns a dummy filename for a textblock so we can tell what file a text block comes from */
void bpy_text_filename_get(char *fn, size_t fn_len, Text *text)
{
  BLI_snprintf(
      fn, fn_len, "%s%c%s", ID_BLEND_PATH(bpy_import_main, &text->id), SEP, text->id.name + 2);
}

bool bpy_text_compile(Text *text)
{
  char fn_dummy[FILE_MAX];
  PyObject *fn_dummy_py;
  char *buf;

  bpy_text_filename_get(fn_dummy, sizeof(fn_dummy), text);

  /* if previously compiled, free the object */
  free_compiled_text(text);

  fn_dummy_py = PyC_UnicodeFromBytes(fn_dummy);

  size_t buf_len_dummy;
  buf = txt_to_buf(text, &buf_len_dummy);
  text->compiled = Py_CompileStringObject(buf, fn_dummy_py, Py_file_input, nullptr, -1);
  MEM_freeN(buf);

  Py_DECREF(fn_dummy_py);

  if (PyErr_Occurred()) {
    PyErr_Print();
    PyErr_Clear();
    PySys_SetObject("last_traceback", nullptr);
    free_compiled_text(text);
    return false;
  }
  else {
    return true;
  }
}

PyObject *bpy_text_import(Text *text)
{
  char modulename[MAX_ID_NAME + 2];
  int len;

  if (!text->compiled) {
    if (bpy_text_compile(text) == false) {
      return nullptr;
    }
  }

  len = strlen(text->id.name + 2);
  BLI_strncpy(modulename, text->id.name + 2, len);
  modulename[len - 3] = '\0'; /* remove .py */
  return PyImport_ExecCodeModule(modulename, (PyObject *)text->compiled);
}

PyObject *bpy_text_import_name(const char *name, int *found)
{
  Text *text;
  char txtname[MAX_ID_NAME - 2];
  int namelen = strlen(name);
  // XXX	Main *maggie = bpy_import_main ? bpy_import_main : G_MAIN;
  Main *maggie = bpy_import_main;

  *found = 0;

  if (!maggie) {
    printf("ERROR: bpy_import_main_set() was not called before running python. this is a bug.\n");
    return nullptr;
  }

  /* we know this cant be importable, the name is too long for blender! */
  if (namelen >= (MAX_ID_NAME - 2) - 3) {
    return nullptr;
  }

  memcpy(txtname, name, namelen);
  memcpy(&txtname[namelen], ".py", 4);

  text = (Text *)BLI_findstring(&maggie->texts, txtname, offsetof(ID, name) + 2);

  if (text) {
    *found = 1;
    return bpy_text_import(text);
  }

  /* If we still haven't found the module try additional modules form bpy_import_main_list */
  maggie = (Main *)bpy_import_main_list.first;
  while (maggie && !text) {
    text = (Text *)BLI_findstring(&maggie->texts, txtname, offsetof(ID, name) + 2);
    maggie = maggie->next;
  }

  if (!text) {
    return nullptr;
  }
  else {
    *found = 1;
  }

  return bpy_text_import(text);
}

static PyObject *blender_import(PyObject */*self*/, PyObject *args, PyObject *kw)
{
  PyObject *exception, *err, *tb;
  const char *name;
  int found = 0;
  PyObject *globals = nullptr, *locals = nullptr, *fromlist = nullptr;
  int level = 0; /* relative imports */
  PyObject *newmodule;

  static const char *_keywords[] = {"name", "globals", "locals", "fromlist", "level", nullptr};
  static _PyArg_Parser _parser = {
      PY_ARG_PARSER_HEAD_COMPAT()
      "s"  /* name */
      "|"  /* Optional */
      "O"  /* globals */
      "O"  /* locals */
      "O"  /* fromlist */
      "i"  /* level */
      ":bpy_import_meth",
      _keywords,
      nullptr,
  };
  if (!_PyArg_ParseTupleAndKeywordsFast(
          args, kw, &_parser, &name, &globals, &locals, &fromlist, &level)) {
    return nullptr;
  }

  /* import existing builtin modules or modules that have been imported already */
  newmodule = PyImport_ImportModuleLevel(name, globals, locals, fromlist, level);

  if (newmodule) {
    return newmodule;
  }

  PyErr_Fetch(&exception,
              &err,
              &tb); /* get the python error in case we cant import as blender text either */

  /* importing from existing modules failed, see if we have this module as blender text */
  newmodule = bpy_text_import_name(name, &found);

  if (newmodule) { /* found module as blender text, ignore above exception */
    PyErr_Clear();
    Py_XDECREF(exception);
    Py_XDECREF(err);
    Py_XDECREF(tb);
    /* printf("imported from text buffer...\n"); */
  }
  else if (found ==
           1) { /* blender text module failed to execute but was found, use its error message */
    Py_XDECREF(exception);
    Py_XDECREF(err);
    Py_XDECREF(tb);

    //PyErr_Format(PyExc_ImportError, "Failed to import module : '%s'", name);

    return nullptr;
  }
  else {
    /* no blender text was found that could import the module
     * reuse the original error from PyImport_ImportModuleEx */
    PyErr_Restore(exception, err, tb);
  }
  return newmodule;
}


PyMethodDef bpy_import_meth = {"bpy_import_meth",
                               (PyCFunction)blender_import,
                               METH_VARARGS | METH_KEYWORDS,
                               "blenders import"};
