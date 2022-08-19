/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup freestyle
 */

#include "BPy_UnaryFunction0DUnsigned.h"

#include "../BPy_Convert.h"
#include "../Iterator/BPy_Interface0DIterator.h"

#include "UnaryFunction0D_unsigned_int/BPy_QuantitativeInvisibilityF0D.h"

#ifdef __cplusplus
extern "C" {
#endif

using namespace Freestyle;

///////////////////////////////////////////////////////////////////////////////////////////

//-------------------MODULE INITIALIZATION--------------------------------

int UnaryFunction0DUnsigned_Init(PyObject *module)
{
  if (module == nullptr) {
    return -1;
  }

  if (PyType_Ready(&UnaryFunction0DUnsigned_Type) < 0) {
    return -1;
  }
  Py_INCREF(&UnaryFunction0DUnsigned_Type);
  PyModule_AddObject(module, "UnaryFunction0DUnsigned", (PyObject *)&UnaryFunction0DUnsigned_Type);

  if (PyType_Ready(&QuantitativeInvisibilityF0D_Type) < 0) {
    return -1;
  }
  Py_INCREF(&QuantitativeInvisibilityF0D_Type);
  PyModule_AddObject(
      module, "QuantitativeInvisibilityF0D", (PyObject *)&QuantitativeInvisibilityF0D_Type);

  return 0;
}

//------------------------INSTANCE METHODS ----------------------------------

static char UnaryFunction0DUnsigned___doc__[] =
    "Class hierarchy: :class:`UnaryFunction0D` > :class:`UnaryFunction0DUnsigned`\n"
    "\n"
    "Base class for unary functions (functors) that work on\n"
    ":class:`Interface0DIterator` and return an int value.\n"
    "\n"
    ".. method:: __init__()\n"
    "\n"
    "   Default constructor.\n";

static int UnaryFunction0DUnsigned___init__(BPy_UnaryFunction0DUnsigned *self,
                                            PyObject *args,
                                            PyObject *kwds)
{
  static const char *kwlist[] = {nullptr};

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "", (char **)kwlist)) {
    return -1;
  }
  self->uf0D_unsigned = new UnaryFunction0D<unsigned int>();
  self->uf0D_unsigned->py_uf0D = (PyObject *)self;
  return 0;
}

static void UnaryFunction0DUnsigned___dealloc__(BPy_UnaryFunction0DUnsigned *self)
{
  delete self->uf0D_unsigned;
  UnaryFunction0D_Type.tp_dealloc((PyObject *)self);
}

static PyObject *UnaryFunction0DUnsigned___repr__(BPy_UnaryFunction0DUnsigned *self)
{
  return PyUnicode_FromFormat(
      "type: %s - address: %p", Py_TYPE(self)->tp_name, self->uf0D_unsigned);
}

static PyObject *UnaryFunction0DUnsigned___call__(BPy_UnaryFunction0DUnsigned *self,
                                                  PyObject *args,
                                                  PyObject *kwds)
{
  static const char *kwlist[] = {"it", nullptr};
  PyObject *obj;

  if (!PyArg_ParseTupleAndKeywords(
          args, kwds, "O!", (char **)kwlist, &Interface0DIterator_Type, &obj)) {
    return nullptr;
  }

  if (typeid(*(self->uf0D_unsigned)) == typeid(UnaryFunction0D<unsigned int>)) {
    PyErr_SetString(PyExc_TypeError, "__call__ method not properly overridden");
    return nullptr;
  }
  if (self->uf0D_unsigned->operator()(*(((BPy_Interface0DIterator *)obj)->if0D_it)) < 0) {
    if (!PyErr_Occurred()) {
      string class_name(Py_TYPE(self)->tp_name);
      PyErr_SetString(PyExc_RuntimeError, (class_name + " __call__ method failed").c_str());
    }
    return nullptr;
  }
  return PyLong_FromLong(self->uf0D_unsigned->result);
}

/*-----------------------BPy_UnaryFunction0DUnsigned type definition ----------------------------*/

PyTypeObject UnaryFunction0DUnsigned_Type = {
    PyVarObject_HEAD_INIT(nullptr, 0) "UnaryFunction0DUnsigned", /* tp_name */
    sizeof(BPy_UnaryFunction0DUnsigned),                         /* tp_basicsize */
    0,                                                           /* tp_itemsize */
    (destructor)UnaryFunction0DUnsigned___dealloc__,             /* tp_dealloc */
    0,                                                           /* tp_vectorcall_offset */
    nullptr,                                                     /* tp_getattr */
    nullptr,                                                     /* tp_setattr */
    nullptr,                                                     /* tp_reserved */
    (reprfunc)UnaryFunction0DUnsigned___repr__,                  /* tp_repr */
    nullptr,                                                     /* tp_as_number */
    nullptr,                                                     /* tp_as_sequence */
    nullptr,                                                     /* tp_as_mapping */
    nullptr,                                                     /* tp_hash */
    (ternaryfunc)UnaryFunction0DUnsigned___call__,               /* tp_call */
    nullptr,                                                     /* tp_str */
    nullptr,                                                     /* tp_getattro */
    nullptr,                                                     /* tp_setattro */
    nullptr,                                                     /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,                    /* tp_flags */
    UnaryFunction0DUnsigned___doc__,                             /* tp_doc */
    nullptr,                                                     /* tp_traverse */
    nullptr,                                                     /* tp_clear */
    nullptr,                                                     /* tp_richcompare */
    0,                                                           /* tp_weaklistoffset */
    nullptr,                                                     /* tp_iter */
    nullptr,                                                     /* tp_iternext */
    nullptr,                                                     /* tp_methods */
    nullptr,                                                     /* tp_members */
    nullptr,                                                     /* tp_getset */
    &UnaryFunction0D_Type,                                       /* tp_base */
    nullptr,                                                     /* tp_dict */
    nullptr,                                                     /* tp_descr_get */
    nullptr,                                                     /* tp_descr_set */
    0,                                                           /* tp_dictoffset */
    (initproc)UnaryFunction0DUnsigned___init__,                  /* tp_init */
    nullptr,                                                     /* tp_alloc */
    nullptr,                                                     /* tp_new */
};

///////////////////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
}
#endif
