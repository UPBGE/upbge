/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup freestyle
 */

#include "BPy_GetYF0D.h"

#include "../../../view_map/Functions0D.h"

#ifdef __cplusplus
extern "C" {
#endif

using namespace Freestyle;

///////////////////////////////////////////////////////////////////////////////////////////

//------------------------INSTANCE METHODS ----------------------------------

static char GetYF0D___doc__[] =
    "Class hierarchy: :class:`freestyle.types.UnaryFunction0D` > "
    ":class:`freestyle.types.UnaryFunction0DDouble` > :class:`GetYF0D`\n"
    "\n"
    ".. method:: __init__()\n"
    "\n"
    "   Builds a GetYF0D object.\n"
    "\n"
    ".. method:: __call__(it)\n"
    "\n"
    "   Returns the Y 3D coordinate of the :class:`freestyle.types.Interface0D` pointed by\n"
    "   the Interface0DIterator.\n"
    "\n"
    "   :arg it: An Interface0DIterator object.\n"
    "   :type it: :class:`freestyle.types.Interface0DIterator`\n"
    "   :return: The Y 3D coordinate of the pointed Interface0D.\n"
    "   :rtype: float\n";

static int GetYF0D___init__(BPy_GetYF0D *self, PyObject *args, PyObject *kwds)
{
  static const char *kwlist[] = {nullptr};

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "", (char **)kwlist)) {
    return -1;
  }
  self->py_uf0D_double.uf0D_double = new Functions0D::GetYF0D();
  self->py_uf0D_double.uf0D_double->py_uf0D = (PyObject *)self;
  return 0;
}

/*-----------------------BPy_GetYF0D type definition ------------------------------*/

PyTypeObject GetYF0D_Type = {
    PyVarObject_HEAD_INIT(nullptr, 0) "GetYF0D", /* tp_name */
    sizeof(BPy_GetYF0D),                         /* tp_basicsize */
    0,                                           /* tp_itemsize */
    nullptr,                                     /* tp_dealloc */
    0,                                           /* tp_vectorcall_offset */
    nullptr,                                     /* tp_getattr */
    nullptr,                                     /* tp_setattr */
    nullptr,                                     /* tp_reserved */
    nullptr,                                     /* tp_repr */
    nullptr,                                     /* tp_as_number */
    nullptr,                                     /* tp_as_sequence */
    nullptr,                                     /* tp_as_mapping */
    nullptr,                                     /* tp_hash */
    nullptr,                                     /* tp_call */
    nullptr,                                     /* tp_str */
    nullptr,                                     /* tp_getattro */
    nullptr,                                     /* tp_setattro */
    nullptr,                                     /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,    /* tp_flags */
    GetYF0D___doc__,                             /* tp_doc */
    nullptr,                                     /* tp_traverse */
    nullptr,                                     /* tp_clear */
    nullptr,                                     /* tp_richcompare */
    0,                                           /* tp_weaklistoffset */
    nullptr,                                     /* tp_iter */
    nullptr,                                     /* tp_iternext */
    nullptr,                                     /* tp_methods */
    nullptr,                                     /* tp_members */
    nullptr,                                     /* tp_getset */
    &UnaryFunction0DDouble_Type,                 /* tp_base */
    nullptr,                                     /* tp_dict */
    nullptr,                                     /* tp_descr_get */
    nullptr,                                     /* tp_descr_set */
    0,                                           /* tp_dictoffset */
    (initproc)GetYF0D___init__,                  /* tp_init */
    nullptr,                                     /* tp_alloc */
    nullptr,                                     /* tp_new */
};

///////////////////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
}
#endif
