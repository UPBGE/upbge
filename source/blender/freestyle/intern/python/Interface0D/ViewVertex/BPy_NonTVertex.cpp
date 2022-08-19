/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup freestyle
 */

#include "BPy_NonTVertex.h"

#include "../../BPy_Convert.h"
#include "../BPy_SVertex.h"

#ifdef __cplusplus
extern "C" {
#endif

using namespace Freestyle;

///////////////////////////////////////////////////////////////////////////////////////////

/*----------------------NonTVertex methods ----------------------------*/

PyDoc_STRVAR(
    NonTVertex_doc,
    "Class hierarchy: :class:`Interface0D` > :class:`ViewVertex` > :class:`NonTVertex`\n"
    "\n"
    "View vertex for corners, cusps, etc. associated to a single SVertex.\n"
    "Can be associated to 2 or more view edges.\n"
    "\n"
    ".. method:: __init__()\n"
    "            __init__(svertex)\n"
    "\n"
    "   Builds a :class:`NonTVertex` using the default constructor or a :class:`SVertex`.\n"
    "\n"
    "   :arg svertex: An SVertex object.\n"
    "   :type svertex: :class:`SVertex`");

/* NOTE: No copy constructor in Python because the C++ copy constructor is 'protected'. */

static int NonTVertex_init(BPy_NonTVertex *self, PyObject *args, PyObject *kwds)
{
  static const char *kwlist[] = {"svertex", nullptr};
  PyObject *obj = nullptr;

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O!", (char **)kwlist, &SVertex_Type, &obj)) {
    return -1;
  }
  if (!obj) {
    self->ntv = new NonTVertex();
  }
  else {
    self->ntv = new NonTVertex(((BPy_SVertex *)obj)->sv);
  }
  self->py_vv.vv = self->ntv;
  self->py_vv.py_if0D.if0D = self->ntv;
  self->py_vv.py_if0D.borrowed = false;
  return 0;
}

/*----------------------NonTVertex get/setters ----------------------------*/

PyDoc_STRVAR(NonTVertex_svertex_doc,
             "The SVertex on top of which this NonTVertex is built.\n"
             "\n"
             ":type: :class:`SVertex`");

static PyObject *NonTVertex_svertex_get(BPy_NonTVertex *self, void *UNUSED(closure))
{
  SVertex *v = self->ntv->svertex();
  if (v) {
    return BPy_SVertex_from_SVertex(*v);
  }
  Py_RETURN_NONE;
}

static int NonTVertex_svertex_set(BPy_NonTVertex *self, PyObject *value, void *UNUSED(closure))
{
  if (!BPy_SVertex_Check(value)) {
    PyErr_SetString(PyExc_TypeError, "value must be an SVertex");
    return -1;
  }
  self->ntv->setSVertex(((BPy_SVertex *)value)->sv);
  return 0;
}

static PyGetSetDef BPy_NonTVertex_getseters[] = {
    {"svertex",
     (getter)NonTVertex_svertex_get,
     (setter)NonTVertex_svertex_set,
     NonTVertex_svertex_doc,
     nullptr},
    {nullptr, nullptr, nullptr, nullptr, nullptr} /* Sentinel */
};

/*-----------------------BPy_NonTVertex type definition ------------------------------*/
PyTypeObject NonTVertex_Type = {
    PyVarObject_HEAD_INIT(nullptr, 0) "NonTVertex", /* tp_name */
    sizeof(BPy_NonTVertex),                         /* tp_basicsize */
    0,                                              /* tp_itemsize */
    nullptr,                                        /* tp_dealloc */
    0,                                              /* tp_vectorcall_offset */
    nullptr,                                        /* tp_getattr */
    nullptr,                                        /* tp_setattr */
    nullptr,                                        /* tp_reserved */
    nullptr,                                        /* tp_repr */
    nullptr,                                        /* tp_as_number */
    nullptr,                                        /* tp_as_sequence */
    nullptr,                                        /* tp_as_mapping */
    nullptr,                                        /* tp_hash */
    nullptr,                                        /* tp_call */
    nullptr,                                        /* tp_str */
    nullptr,                                        /* tp_getattro */
    nullptr,                                        /* tp_setattro */
    nullptr,                                        /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,       /* tp_flags */
    NonTVertex_doc,                                 /* tp_doc */
    nullptr,                                        /* tp_traverse */
    nullptr,                                        /* tp_clear */
    nullptr,                                        /* tp_richcompare */
    0,                                              /* tp_weaklistoffset */
    nullptr,                                        /* tp_iter */
    nullptr,                                        /* tp_iternext */
    nullptr,                                        /* tp_methods */
    nullptr,                                        /* tp_members */
    BPy_NonTVertex_getseters,                       /* tp_getset */
    &ViewVertex_Type,                               /* tp_base */
    nullptr,                                        /* tp_dict */
    nullptr,                                        /* tp_descr_get */
    nullptr,                                        /* tp_descr_set */
    0,                                              /* tp_dictoffset */
    (initproc)NonTVertex_init,                      /* tp_init */
    nullptr,                                        /* tp_alloc */
    nullptr,                                        /* tp_new */
};

///////////////////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
}
#endif
