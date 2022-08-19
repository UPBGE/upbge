/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup pythonintern
 *
 * This file defines '_bpy_path' module, Some 'C' functionality used by 'bpy.path'
 */

#include <Python.h>

#include "BLI_utildefines.h"

#include "bpy_path.h"

#include "../generic/py_capi_utils.h"

/* #include "IMB_imbuf_types.h" */
extern const char *imb_ext_image[];
extern const char *imb_ext_movie[];
extern const char *imb_ext_audio[];

/*----------------------------MODULE INIT-------------------------*/
static struct PyModuleDef _bpy_path_module_def = {
    PyModuleDef_HEAD_INIT,
    "_bpy_path", /* m_name */
    NULL,        /* m_doc */
    0,           /* m_size */
    NULL,        /* m_methods */
    NULL,        /* m_reload */
    NULL,        /* m_traverse */
    NULL,        /* m_clear */
    NULL,        /* m_free */
};

PyObject *BPyInit__bpy_path(void)
{
  PyObject *submodule;

  submodule = PyModule_Create(&_bpy_path_module_def);

  PyModule_AddObject(submodule, "extensions_image", PyC_FrozenSetFromStrings(imb_ext_image));
  PyModule_AddObject(submodule, "extensions_movie", PyC_FrozenSetFromStrings(imb_ext_movie));
  PyModule_AddObject(submodule, "extensions_audio", PyC_FrozenSetFromStrings(imb_ext_audio));

  return submodule;
}
