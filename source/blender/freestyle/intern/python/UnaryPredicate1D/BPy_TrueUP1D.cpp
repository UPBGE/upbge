/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup freestyle
 */

#include "BPy_TrueUP1D.h"

#ifdef __cplusplus
extern "C" {
#endif

using namespace Freestyle;

///////////////////////////////////////////////////////////////////////////////////////////

//------------------------INSTANCE METHODS ----------------------------------

static char TrueUP1D___doc__[] =
    "Class hierarchy: :class:`freestyle.types.UnaryPredicate1D` > :class:`TrueUP1D`\n"
    "\n"
    ".. method:: __call__(inter)\n"
    "\n"
    "   Always returns true.\n"
    "\n"
    "   :arg inter: An Interface1D object.\n"
    "   :type inter: :class:`freestyle.types.Interface1D`\n"
    "   :return: True.\n"
    "   :rtype: bool\n";

static int TrueUP1D___init__(BPy_TrueUP1D *self, PyObject *args, PyObject *kwds)
{
  static const char *kwlist[] = {nullptr};

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "", (char **)kwlist)) {
    return -1;
  }
  self->py_up1D.up1D = new Predicates1D::TrueUP1D();
  return 0;
}

/*-----------------------BPy_TrueUP1D type definition ------------------------------*/

PyTypeObject TrueUP1D_Type = {
    PyVarObject_HEAD_INIT(nullptr, 0) "TrueUP1D", /* tp_name */
    sizeof(BPy_TrueUP1D),                         /* tp_basicsize */
    0,                                            /* tp_itemsize */
    nullptr,                                      /* tp_dealloc */
    0,                                            /* tp_vectorcall_offset */
    nullptr,                                      /* tp_getattr */
    nullptr,                                      /* tp_setattr */
    nullptr,                                      /* tp_reserved */
    nullptr,                                      /* tp_repr */
    nullptr,                                      /* tp_as_number */
    nullptr,                                      /* tp_as_sequence */
    nullptr,                                      /* tp_as_mapping */
    nullptr,                                      /* tp_hash */
    nullptr,                                      /* tp_call */
    nullptr,                                      /* tp_str */
    nullptr,                                      /* tp_getattro */
    nullptr,                                      /* tp_setattro */
    nullptr,                                      /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,     /* tp_flags */
    TrueUP1D___doc__,                             /* tp_doc */
    nullptr,                                      /* tp_traverse */
    nullptr,                                      /* tp_clear */
    nullptr,                                      /* tp_richcompare */
    0,                                            /* tp_weaklistoffset */
    nullptr,                                      /* tp_iter */
    nullptr,                                      /* tp_iternext */
    nullptr,                                      /* tp_methods */
    nullptr,                                      /* tp_members */
    nullptr,                                      /* tp_getset */
    &UnaryPredicate1D_Type,                       /* tp_base */
    nullptr,                                      /* tp_dict */
    nullptr,                                      /* tp_descr_get */
    nullptr,                                      /* tp_descr_set */
    0,                                            /* tp_dictoffset */
    (initproc)TrueUP1D___init__,                  /* tp_init */
    nullptr,                                      /* tp_alloc */
    nullptr,                                      /* tp_new */
};

///////////////////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
}
#endif
