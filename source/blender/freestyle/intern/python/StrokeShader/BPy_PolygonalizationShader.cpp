/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup freestyle
 */

#include "BPy_PolygonalizationShader.h"

#include "../../stroke/BasicStrokeShaders.h"

#ifdef __cplusplus
extern "C" {
#endif

using namespace Freestyle;

///////////////////////////////////////////////////////////////////////////////////////////

//------------------------INSTANCE METHODS ----------------------------------

static char PolygonalizationShader___doc__[] =
    "Class hierarchy: :class:`freestyle.types.StrokeShader` > :class:`PolygonalizationShader`\n"
    "\n"
    "[Geometry shader]\n"
    "\n"
    ".. method:: __init__(error)\n"
    "\n"
    "   Builds a PolygonalizationShader object.\n"
    "\n"
    "   :arg error: The error we want our polygonal approximation to have\n"
    "      with respect to the original geometry.  The smaller, the closer\n"
    "      the new stroke is to the original one.  This error corresponds to\n"
    "      the maximum distance between the new stroke and the old one.\n"
    "   :type error: float\n"
    "\n"
    ".. method:: shade(stroke)\n"
    "\n"
    "   Modifies the Stroke geometry so that it looks more \"polygonal\".\n"
    "   The basic idea is to start from the minimal stroke approximation\n"
    "   consisting in a line joining the first vertex to the last one and\n"
    "   to subdivide using the original stroke vertices until a certain\n"
    "   error is reached.\n"
    "\n"
    "   :arg stroke: A Stroke object.\n"
    "   :type stroke: :class:`freestyle.types.Stroke`\n";

static int PolygonalizationShader___init__(BPy_PolygonalizationShader *self,
                                           PyObject *args,
                                           PyObject *kwds)
{
  static const char *kwlist[] = {"error", nullptr};
  float f;

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "f", (char **)kwlist, &f)) {
    return -1;
  }
  self->py_ss.ss = new StrokeShaders::PolygonalizationShader(f);
  return 0;
}

/*-----------------------BPy_PolygonalizationShader type definition -----------------------------*/

PyTypeObject PolygonalizationShader_Type = {
    PyVarObject_HEAD_INIT(nullptr, 0) "PolygonalizationShader", /* tp_name */
    sizeof(BPy_PolygonalizationShader),                         /* tp_basicsize */
    0,                                                          /* tp_itemsize */
    nullptr,                                                    /* tp_dealloc */
    0,                                                          /* tp_vectorcall_offset */
    nullptr,                                                    /* tp_getattr */
    nullptr,                                                    /* tp_setattr */
    nullptr,                                                    /* tp_reserved */
    nullptr,                                                    /* tp_repr */
    nullptr,                                                    /* tp_as_number */
    nullptr,                                                    /* tp_as_sequence */
    nullptr,                                                    /* tp_as_mapping */
    nullptr,                                                    /* tp_hash */
    nullptr,                                                    /* tp_call */
    nullptr,                                                    /* tp_str */
    nullptr,                                                    /* tp_getattro */
    nullptr,                                                    /* tp_setattro */
    nullptr,                                                    /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,                   /* tp_flags */
    PolygonalizationShader___doc__,                             /* tp_doc */
    nullptr,                                                    /* tp_traverse */
    nullptr,                                                    /* tp_clear */
    nullptr,                                                    /* tp_richcompare */
    0,                                                          /* tp_weaklistoffset */
    nullptr,                                                    /* tp_iter */
    nullptr,                                                    /* tp_iternext */
    nullptr,                                                    /* tp_methods */
    nullptr,                                                    /* tp_members */
    nullptr,                                                    /* tp_getset */
    &StrokeShader_Type,                                         /* tp_base */
    nullptr,                                                    /* tp_dict */
    nullptr,                                                    /* tp_descr_get */
    nullptr,                                                    /* tp_descr_set */
    0,                                                          /* tp_dictoffset */
    (initproc)PolygonalizationShader___init__,                  /* tp_init */
    nullptr,                                                    /* tp_alloc */
    nullptr,                                                    /* tp_new */
};

///////////////////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
}
#endif
