/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup freestyle
 */

#include "BPy_MaterialF0D.h"

#include "../../../view_map/Functions0D.h"

#ifdef __cplusplus
extern "C" {
#endif

using namespace Freestyle;

///////////////////////////////////////////////////////////////////////////////////////////

//------------------------INSTANCE METHODS ----------------------------------

static char MaterialF0D___doc__[] =
    "Class hierarchy: :class:`freestyle.types.UnaryFunction0D` > "
    ":class:`freestyle.types.UnaryFunction0DMaterial` > :class:`MaterialF0D`\n"
    "\n"
    ".. method:: __init__()\n"
    "\n"
    "   Builds a MaterialF0D object.\n"
    "\n"
    ".. method:: __call__(it)\n"
    "\n"
    "   Returns the material of the object evaluated at the\n"
    "   :class:`freestyle.types.Interface0D` pointed by the\n"
    "   Interface0DIterator.  This evaluation can be ambiguous (in the case of\n"
    "   a :class:`freestyle.types.TVertex` for example.  This functor tries to\n"
    "   remove this ambiguity using the context offered by the 1D element to\n"
    "   which the Interface0DIterator belongs to and by arbitrary choosing the\n"
    "   material of the face that lies on its left when following the 1D\n"
    "   element if there are two different materials on each side of the\n"
    "   point.  However, there still can be problematic cases, and the user\n"
    "   willing to deal with this cases in a specific way should implement its\n"
    "   own getMaterial functor.\n"
    "\n"
    "   :arg it: An Interface0DIterator object.\n"
    "   :type it: :class:`freestyle.types.Interface0DIterator`\n"
    "   :return: The material of the object evaluated at the pointed\n"
    "      Interface0D.\n"
    "   :rtype: :class:`freestyle.types.Material`\n";

static int MaterialF0D___init__(BPy_MaterialF0D *self, PyObject *args, PyObject *kwds)
{
  static const char *kwlist[] = {nullptr};

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "", (char **)kwlist)) {
    return -1;
  }
  self->py_uf0D_material.uf0D_material = new Functions0D::MaterialF0D();
  self->py_uf0D_material.uf0D_material->py_uf0D = (PyObject *)self;
  return 0;
}

/*-----------------------BPy_MaterialF0D type definition ------------------------------*/

PyTypeObject MaterialF0D_Type = {
    PyVarObject_HEAD_INIT(nullptr, 0) "MaterialF0D", /* tp_name */
    sizeof(BPy_MaterialF0D),                         /* tp_basicsize */
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
    MaterialF0D___doc__,                             /* tp_doc */
    nullptr,                                         /* tp_traverse */
    nullptr,                                         /* tp_clear */
    nullptr,                                         /* tp_richcompare */
    0,                                               /* tp_weaklistoffset */
    nullptr,                                         /* tp_iter */
    nullptr,                                         /* tp_iternext */
    nullptr,                                         /* tp_methods */
    nullptr,                                         /* tp_members */
    nullptr,                                         /* tp_getset */
    &UnaryFunction0DMaterial_Type,                   /* tp_base */
    nullptr,                                         /* tp_dict */
    nullptr,                                         /* tp_descr_get */
    nullptr,                                         /* tp_descr_set */
    0,                                               /* tp_dictoffset */
    (initproc)MaterialF0D___init__,                  /* tp_init */
    nullptr,                                         /* tp_alloc */
    nullptr,                                         /* tp_new */
};

///////////////////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
}
#endif
