/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup freestyle
 */

#include "BPy_ConstrainedIncreasingThicknessShader.h"

#include "../../stroke/BasicStrokeShaders.h"

#ifdef __cplusplus
extern "C" {
#endif

using namespace Freestyle;

///////////////////////////////////////////////////////////////////////////////////////////

//------------------------INSTANCE METHODS ----------------------------------

static char ConstrainedIncreasingThicknessShader___doc__[] =
    "Class hierarchy: :class:`freestyle.types.StrokeShader` > "
    ":class:`ConstrainedIncreasingThicknessShader`\n"
    "\n"
    "[Thickness shader]\n"
    "\n"
    ".. method:: __init__(thickness_min, thickness_max, ratio)\n"
    "\n"
    "   Builds a ConstrainedIncreasingThicknessShader object.\n"
    "\n"
    "   :arg thickness_min: The minimum thickness.\n"
    "   :type thickness_min: float\n"
    "   :arg thickness_max: The maximum thickness.\n"
    "   :type thickness_max: float\n"
    "   :arg ratio: The thickness/length ratio that we don't want to exceed. \n"
    "   :type ratio: float\n"
    "\n"
    ".. method:: shade(stroke)\n"
    "\n"
    "   Same as the :class:`IncreasingThicknessShader`, but here we allow\n"
    "   the user to control the thickness/length ratio so that we don't get\n"
    "   fat short lines.\n"
    "\n"
    "   :arg stroke: A Stroke object.\n"
    "   :type stroke: :class:`freestyle.types.Stroke`\n";

static int ConstrainedIncreasingThicknessShader___init__(
    BPy_ConstrainedIncreasingThicknessShader *self, PyObject *args, PyObject *kwds)
{
  static const char *kwlist[] = {"thickness_min", "thickness_max", "ratio", nullptr};
  float f1, f2, f3;

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "fff", (char **)kwlist, &f1, &f2, &f3)) {
    return -1;
  }
  self->py_ss.ss = new StrokeShaders::ConstrainedIncreasingThicknessShader(f1, f2, f3);
  return 0;
}

/*-----------------------BPy_ConstrainedIncreasingThicknessShader type definition ---------------*/

PyTypeObject ConstrainedIncreasingThicknessShader_Type = {
    PyVarObject_HEAD_INIT(nullptr, 0) "ConstrainedIncreasingThicknessShader", /* tp_name */
    sizeof(BPy_ConstrainedIncreasingThicknessShader),                         /* tp_basicsize */
    0,                                                                        /* tp_itemsize */
    nullptr,                                                                  /* tp_dealloc */
    0,                                                       /* tp_vectorcall_offset */
    nullptr,                                                 /* tp_getattr */
    nullptr,                                                 /* tp_setattr */
    nullptr,                                                 /* tp_reserved */
    nullptr,                                                 /* tp_repr */
    nullptr,                                                 /* tp_as_number */
    nullptr,                                                 /* tp_as_sequence */
    nullptr,                                                 /* tp_as_mapping */
    nullptr,                                                 /* tp_hash */
    nullptr,                                                 /* tp_call */
    nullptr,                                                 /* tp_str */
    nullptr,                                                 /* tp_getattro */
    nullptr,                                                 /* tp_setattro */
    nullptr,                                                 /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,                /* tp_flags */
    ConstrainedIncreasingThicknessShader___doc__,            /* tp_doc */
    nullptr,                                                 /* tp_traverse */
    nullptr,                                                 /* tp_clear */
    nullptr,                                                 /* tp_richcompare */
    0,                                                       /* tp_weaklistoffset */
    nullptr,                                                 /* tp_iter */
    nullptr,                                                 /* tp_iternext */
    nullptr,                                                 /* tp_methods */
    nullptr,                                                 /* tp_members */
    nullptr,                                                 /* tp_getset */
    &StrokeShader_Type,                                      /* tp_base */
    nullptr,                                                 /* tp_dict */
    nullptr,                                                 /* tp_descr_get */
    nullptr,                                                 /* tp_descr_set */
    0,                                                       /* tp_dictoffset */
    (initproc)ConstrainedIncreasingThicknessShader___init__, /* tp_init */
    nullptr,                                                 /* tp_alloc */
    nullptr,                                                 /* tp_new */
};

///////////////////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
}
#endif
