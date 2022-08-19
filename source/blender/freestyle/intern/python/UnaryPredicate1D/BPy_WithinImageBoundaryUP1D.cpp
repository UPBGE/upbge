/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup freestyle
 */

#include "BPy_WithinImageBoundaryUP1D.h"

#ifdef __cplusplus
extern "C" {
#endif

using namespace Freestyle;

///////////////////////////////////////////////////////////////////////////////////////////

//------------------------INSTANCE METHODS ----------------------------------

static char WithinImageBoundaryUP1D___doc__[] =
    "Class hierarchy: :class:`freestyle.types.UnaryPredicate1D` > "
    ":class:`WithinImageBoundaryUP1D`\n"
    "\n"
    ".. method:: __init__(xmin, ymin, xmax, ymax)\n"
    "\n"
    "   Builds an WithinImageBoundaryUP1D object.\n"
    "\n"
    "   :arg xmin: X lower bound of the image boundary.\n"
    "   :type xmin: float\n"
    "   :arg ymin: Y lower bound of the image boundary.\n"
    "   :type ymin: float\n"
    "   :arg xmax: X upper bound of the image boundary.\n"
    "   :type xmax: float\n"
    "   :arg ymax: Y upper bound of the image boundary.\n"
    "   :type ymax: float\n"
    "\n"
    ".. method:: __call__(inter)\n"
    "\n"
    "   Returns true if the Interface1D intersects with image boundary.\n";

static int WithinImageBoundaryUP1D___init__(BPy_WithinImageBoundaryUP1D *self,
                                            PyObject *args,
                                            PyObject *kwds)
{
  static const char *kwlist[] = {"xmin", "ymin", "xmax", "ymax", nullptr};
  double xmin, ymin, xmax, ymax;

  if (!PyArg_ParseTupleAndKeywords(
          args, kwds, "dddd", (char **)kwlist, &xmin, &ymin, &xmax, &ymax)) {
    return -1;
  }
  self->py_up1D.up1D = new Predicates1D::WithinImageBoundaryUP1D(xmin, ymin, xmax, ymax);
  return 0;
}

/*-----------------------BPy_TrueUP1D type definition ------------------------------*/

PyTypeObject WithinImageBoundaryUP1D_Type = {
    PyVarObject_HEAD_INIT(nullptr, 0) "WithinImageBoundaryUP1D", /* tp_name */
    sizeof(BPy_WithinImageBoundaryUP1D),                         /* tp_basicsize */
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
    WithinImageBoundaryUP1D___doc__,                             /* tp_doc */
    nullptr,                                                     /* tp_traverse */
    nullptr,                                                     /* tp_clear */
    nullptr,                                                     /* tp_richcompare */
    0,                                                           /* tp_weaklistoffset */
    nullptr,                                                     /* tp_iter */
    nullptr,                                                     /* tp_iternext */
    nullptr,                                                     /* tp_methods */
    nullptr,                                                     /* tp_members */
    nullptr,                                                     /* tp_getset */
    &UnaryPredicate1D_Type,                                      /* tp_base */
    nullptr,                                                     /* tp_dict */
    nullptr,                                                     /* tp_descr_get */
    nullptr,                                                     /* tp_descr_set */
    0,                                                           /* tp_dictoffset */
    (initproc)WithinImageBoundaryUP1D___init__,                  /* tp_init */
    nullptr,                                                     /* tp_alloc */
    nullptr,                                                     /* tp_new */
};

///////////////////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
}
#endif
