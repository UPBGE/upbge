/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup freestyle
 */

#include "BPy_GetOccludersF0D.h"

#include "../../../view_map/Functions0D.h"

#ifdef __cplusplus
extern "C" {
#endif

using namespace Freestyle;

///////////////////////////////////////////////////////////////////////////////////////////

//------------------------INSTANCE METHODS ----------------------------------

static char GetOccludersF0D___doc__[] =
    "Class hierarchy: :class:`freestyle.types.UnaryFunction0D` > "
    ":class:`freestyle.types.UnaryFunction0DVectorViewShape` > :class:`GetOccludersF0D`\n"
    "\n"
    ".. method:: __init__()\n"
    "\n"
    "   Builds a GetOccludersF0D object.\n"
    "\n"
    ".. method:: __call__(it)\n"
    "\n"
    "   Returns a list of :class:`freestyle.types.ViewShape` objects occluding the\n"
    "   :class:`freestyle.types.Interface0D` pointed by the Interface0DIterator.\n"
    "\n"
    "   :arg it: An Interface0DIterator object.\n"
    "   :type it: :class:`freestyle.types.Interface0DIterator`\n"
    "   :return: A list of ViewShape objects occluding the pointed\n"
    "      Interface0D.\n"
    "   :rtype: list of :class:`freestyle.types.ViewShape` objects\n";

static int GetOccludersF0D___init__(BPy_GetOccludersF0D *self, PyObject *args, PyObject *kwds)
{
  static const char *kwlist[] = {nullptr};

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "", (char **)kwlist)) {
    return -1;
  }
  self->py_uf0D_vectorviewshape.uf0D_vectorviewshape = new Functions0D::GetOccludersF0D();
  self->py_uf0D_vectorviewshape.uf0D_vectorviewshape->py_uf0D = (PyObject *)self;
  return 0;
}

/*-----------------------BPy_GetOccludersF0D type definition ------------------------------*/

PyTypeObject GetOccludersF0D_Type = {
    PyVarObject_HEAD_INIT(nullptr, 0) "GetOccludersF0D", /* tp_name */
    sizeof(BPy_GetOccludersF0D),                         /* tp_basicsize */
    0,                                                   /* tp_itemsize */
    nullptr,                                             /* tp_dealloc */
    0,                                                   /* tp_vectorcall_offset */
    nullptr,                                             /* tp_getattr */
    nullptr,                                             /* tp_setattr */
    nullptr,                                             /* tp_reserved */
    nullptr,                                             /* tp_repr */
    nullptr,                                             /* tp_as_number */
    nullptr,                                             /* tp_as_sequence */
    nullptr,                                             /* tp_as_mapping */
    nullptr,                                             /* tp_hash */
    nullptr,                                             /* tp_call */
    nullptr,                                             /* tp_str */
    nullptr,                                             /* tp_getattro */
    nullptr,                                             /* tp_setattro */
    nullptr,                                             /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,            /* tp_flags */
    GetOccludersF0D___doc__,                             /* tp_doc */
    nullptr,                                             /* tp_traverse */
    nullptr,                                             /* tp_clear */
    nullptr,                                             /* tp_richcompare */
    0,                                                   /* tp_weaklistoffset */
    nullptr,                                             /* tp_iter */
    nullptr,                                             /* tp_iternext */
    nullptr,                                             /* tp_methods */
    nullptr,                                             /* tp_members */
    nullptr,                                             /* tp_getset */
    &UnaryFunction0DVectorViewShape_Type,                /* tp_base */
    nullptr,                                             /* tp_dict */
    nullptr,                                             /* tp_descr_get */
    nullptr,                                             /* tp_descr_set */
    0,                                                   /* tp_dictoffset */
    (initproc)GetOccludersF0D___init__,                  /* tp_init */
    nullptr,                                             /* tp_alloc */
    nullptr,                                             /* tp_new */
};

///////////////////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
}
#endif
