/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup freestyle
 */

#include "BPy_StrokeTextureStepShader.h"

#include "../../stroke/BasicStrokeShaders.h"

#ifdef __cplusplus
extern "C" {
#endif

using namespace Freestyle;

///////////////////////////////////////////////////////////////////////////////////////////

//------------------------INSTANCE METHODS ----------------------------------

static char StrokeTextureStepShader___doc__[] =
    "Class hierarchy: :class:`freestyle.types.StrokeShader` > :class:`StrokeTextureStepShader`\n"
    "\n"
    "[Texture shader]\n"
    "\n"
    ".. method:: __init__(step)\n"
    "\n"
    "   Builds a StrokeTextureStepShader object.\n"
    "\n"
    "   :arg step: The spacing along the stroke.\n"
    "   :type step: float\n"
    "\n"
    ".. method:: shade(stroke)\n"
    "\n"
    "   Assigns a spacing factor to the texture coordinates of the Stroke.\n"
    "\n"
    "   :arg stroke: A Stroke object.\n"
    "   :type stroke: :class:`freestyle.types.Stroke`\n";

static int StrokeTextureStepShader___init__(BPy_StrokeTextureStepShader *self,
                                            PyObject *args,
                                            PyObject *kwds)
{
  static const char *kwlist[] = {"step", nullptr};
  float step = 0.1;

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "f", (char **)kwlist, &step)) {
    return -1;
  }
  self->py_ss.ss = new StrokeShaders::StrokeTextureStepShader(step);
  return 0;
}

/*-----------------------BPy_StrokeTextureStepShader type definition ----------------------------*/

PyTypeObject StrokeTextureStepShader_Type = {
    PyVarObject_HEAD_INIT(nullptr, 0) "StrokeTextureStepShader", /* tp_name */
    sizeof(BPy_StrokeTextureStepShader),                         /* tp_basicsize */
    0,                                                           /* tp_itemsize */
    nullptr,                                                     /* tp_dealloc */
    0,                                                           /* tp_vectorcall_offset */
    nullptr,                                                     /* tp_getattr */
    nullptr,                                                     /* tp_setattr */
    nullptr,                                                     /* tp_reserved */
    nullptr,                                                     /* tp_repr */
    nullptr,                                                     /* tp_as_number */
    nullptr,                                                     /* tp_as_sequence */
    nullptr,                                                     /* tp_as_mapping */
    nullptr,                                                     /* tp_hash */
    nullptr,                                                     /* tp_call */
    nullptr,                                                     /* tp_str */
    nullptr,                                                     /* tp_getattro */
    nullptr,                                                     /* tp_setattro */
    nullptr,                                                     /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,                    /* tp_flags */
    StrokeTextureStepShader___doc__,                             /* tp_doc */
    nullptr,                                                     /* tp_traverse */
    nullptr,                                                     /* tp_clear */
    nullptr,                                                     /* tp_richcompare */
    0,                                                           /* tp_weaklistoffset */
    nullptr,                                                     /* tp_iter */
    nullptr,                                                     /* tp_iternext */
    nullptr,                                                     /* tp_methods */
    nullptr,                                                     /* tp_members */
    nullptr,                                                     /* tp_getset */
    &StrokeShader_Type,                                          /* tp_base */
    nullptr,                                                     /* tp_dict */
    nullptr,                                                     /* tp_descr_get */
    nullptr,                                                     /* tp_descr_set */
    0,                                                           /* tp_dictoffset */
    (initproc)StrokeTextureStepShader___init__,                  /* tp_init */
    nullptr,                                                     /* tp_alloc */
    nullptr,                                                     /* tp_new */
};

///////////////////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
}
#endif
