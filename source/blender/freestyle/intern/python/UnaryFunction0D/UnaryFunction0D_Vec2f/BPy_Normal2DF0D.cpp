/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup freestyle
 */

#include "BPy_Normal2DF0D.h"

#include "../../../view_map/Functions0D.h"

#ifdef __cplusplus
extern "C" {
#endif

using namespace Freestyle;

///////////////////////////////////////////////////////////////////////////////////////////

//------------------------INSTANCE METHODS ----------------------------------

static char Normal2DF0D___doc__[] =
    "Class hierarchy: :class:`freestyle.types.UnaryFunction0D` > "
    ":class:`freestyle.types.UnaryFunction0DVec2f` > :class:`Normal2DF0D`\n"
    "\n"
    ".. method:: __init__()\n"
    "\n"
    "   Builds a Normal2DF0D object.\n"
    "\n"
    ".. method:: __call__(it)\n"
    "\n"
    "   Returns a two-dimensional vector giving the normalized 2D normal to\n"
    "   the 1D element to which the :class:`freestyle.types.Interface0D`\n"
    "   pointed by the Interface0DIterator belongs.  The normal is evaluated\n"
    "   at the pointed Interface0D.\n"
    "\n"
    "   :arg it: An Interface0DIterator object.\n"
    "   :type it: :class:`freestyle.types.Interface0DIterator`\n"
    "   :return: The 2D normal of the 1D element evaluated at the pointed\n"
    "      Interface0D.\n"
    "   :rtype: :class:`mathutils.Vector`\n";

static int Normal2DF0D___init__(BPy_Normal2DF0D *self, PyObject *args, PyObject *kwds)
{
  static const char *kwlist[] = {nullptr};

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "", (char **)kwlist)) {
    return -1;
  }
  self->py_uf0D_vec2f.uf0D_vec2f = new Functions0D::Normal2DF0D();
  self->py_uf0D_vec2f.uf0D_vec2f->py_uf0D = (PyObject *)self;
  return 0;
}

/*-----------------------BPy_Normal2DF0D type definition ------------------------------*/

PyTypeObject Normal2DF0D_Type = {
    PyVarObject_HEAD_INIT(nullptr, 0) "Normal2DF0D", /* tp_name */
    sizeof(BPy_Normal2DF0D),                         /* tp_basicsize */
    0,                                               /* tp_itemsize */
    nullptr,                                         /* tp_dealloc */
    0,                                               /* tp_vectorcall_offset */
    nullptr,                                         /* tp_getattr */
    nullptr,                                         /* tp_setattr */
    nullptr,                                         /* tp_reserved */
    nullptr,                                         /* tp_repr */
    nullptr,                                         /* tp_as_number */
    nullptr,                                         /* tp_as_sequence */
    nullptr,                                         /* tp_as_mapping */
    nullptr,                                         /* tp_hash */
    nullptr,                                         /* tp_call */
    nullptr,                                         /* tp_str */
    nullptr,                                         /* tp_getattro */
    nullptr,                                         /* tp_setattro */
    nullptr,                                         /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,        /* tp_flags */
    Normal2DF0D___doc__,                             /* tp_doc */
    nullptr,                                         /* tp_traverse */
    nullptr,                                         /* tp_clear */
    nullptr,                                         /* tp_richcompare */
    0,                                               /* tp_weaklistoffset */
    nullptr,                                         /* tp_iter */
    nullptr,                                         /* tp_iternext */
    nullptr,                                         /* tp_methods */
    nullptr,                                         /* tp_members */
    nullptr,                                         /* tp_getset */
    &UnaryFunction0DVec2f_Type,                      /* tp_base */
    nullptr,                                         /* tp_dict */
    nullptr,                                         /* tp_descr_get */
    nullptr,                                         /* tp_descr_set */
    0,                                               /* tp_dictoffset */
    (initproc)Normal2DF0D___init__,                  /* tp_init */
    nullptr,                                         /* tp_alloc */
    nullptr,                                         /* tp_new */
};

///////////////////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
}
#endif
