/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup freestyle
 */

#include "BPy_UnaryFunction1DVec3f.h"

#include "../BPy_Convert.h"
#include "../BPy_IntegrationType.h"
#include "../BPy_Interface1D.h"

#include "UnaryFunction1D_Vec3f/BPy_Orientation3DF1D.h"

#ifdef __cplusplus
extern "C" {
#endif

using namespace Freestyle;

///////////////////////////////////////////////////////////////////////////////////////////

//-------------------MODULE INITIALIZATION--------------------------------

int UnaryFunction1DVec3f_Init(PyObject *module)
{
  if (module == nullptr) {
    return -1;
  }

  if (PyType_Ready(&UnaryFunction1DVec3f_Type) < 0) {
    return -1;
  }
  Py_INCREF(&UnaryFunction1DVec3f_Type);
  PyModule_AddObject(module, "UnaryFunction1DVec3f", (PyObject *)&UnaryFunction1DVec3f_Type);

  if (PyType_Ready(&Orientation3DF1D_Type) < 0) {
    return -1;
  }
  Py_INCREF(&Orientation3DF1D_Type);
  PyModule_AddObject(module, "Orientation3DF1D", (PyObject *)&Orientation3DF1D_Type);

  return 0;
}

//------------------------INSTANCE METHODS ----------------------------------

static char UnaryFunction1DVec3f___doc__[] =
    "Class hierarchy: :class:`UnaryFunction1D` > :class:`UnaryFunction1DVec3f`\n"
    "\n"
    "Base class for unary functions (functors) that work on\n"
    ":class:`Interface1D` and return a 3D vector.\n"
    "\n"
    ".. method:: __init__()\n"
    "            __init__(integration_type)\n"
    "\n"
    "   Builds a unary 1D function using the default constructor\n"
    "   or the integration method given as an argument.\n"
    "\n"
    "   :arg integration_type: An integration method.\n"
    "   :type integration_type: :class:`IntegrationType`\n";

static int UnaryFunction1DVec3f___init__(BPy_UnaryFunction1DVec3f *self,
                                         PyObject *args,
                                         PyObject *kwds)
{
  static const char *kwlist[] = {"integration", nullptr};
  PyObject *obj = nullptr;

  if (!PyArg_ParseTupleAndKeywords(
          args, kwds, "|O!", (char **)kwlist, &IntegrationType_Type, &obj)) {
    return -1;
  }

  if (!obj) {
    self->uf1D_vec3f = new UnaryFunction1D<Vec3f>();
  }
  else {
    self->uf1D_vec3f = new UnaryFunction1D<Vec3f>(IntegrationType_from_BPy_IntegrationType(obj));
  }

  self->uf1D_vec3f->py_uf1D = (PyObject *)self;

  return 0;
}

static void UnaryFunction1DVec3f___dealloc__(BPy_UnaryFunction1DVec3f *self)
{
  delete self->uf1D_vec3f;
  UnaryFunction1D_Type.tp_dealloc((PyObject *)self);
}

static PyObject *UnaryFunction1DVec3f___repr__(BPy_UnaryFunction1DVec3f *self)
{
  return PyUnicode_FromFormat("type: %s - address: %p", Py_TYPE(self)->tp_name, self->uf1D_vec3f);
}

static PyObject *UnaryFunction1DVec3f___call__(BPy_UnaryFunction1DVec3f *self,
                                               PyObject *args,
                                               PyObject *kwds)
{
  static const char *kwlist[] = {"inter", nullptr};
  PyObject *obj = nullptr;

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "O!", (char **)kwlist, &Interface1D_Type, &obj)) {
    return nullptr;
  }

  if (typeid(*(self->uf1D_vec3f)) == typeid(UnaryFunction1D<Vec3f>)) {
    PyErr_SetString(PyExc_TypeError, "__call__ method not properly overridden");
    return nullptr;
  }
  if (self->uf1D_vec3f->operator()(*(((BPy_Interface1D *)obj)->if1D)) < 0) {
    if (!PyErr_Occurred()) {
      string class_name(Py_TYPE(self)->tp_name);
      PyErr_SetString(PyExc_RuntimeError, (class_name + " __call__ method failed").c_str());
    }
    return nullptr;
  }
  return Vector_from_Vec3f(self->uf1D_vec3f->result);
}

/*----------------------UnaryFunction1DVec3f get/setters ----------------------------*/

PyDoc_STRVAR(integration_type_doc,
             "The integration method.\n"
             "\n"
             ":type: :class:`IntegrationType`");

static PyObject *integration_type_get(BPy_UnaryFunction1DVec3f *self, void *UNUSED(closure))
{
  return BPy_IntegrationType_from_IntegrationType(self->uf1D_vec3f->getIntegrationType());
}

static int integration_type_set(BPy_UnaryFunction1DVec3f *self,
                                PyObject *value,
                                void *UNUSED(closure))
{
  if (!BPy_IntegrationType_Check(value)) {
    PyErr_SetString(PyExc_TypeError, "value must be an IntegrationType");
    return -1;
  }
  self->uf1D_vec3f->setIntegrationType(IntegrationType_from_BPy_IntegrationType(value));
  return 0;
}

static PyGetSetDef BPy_UnaryFunction1DVec3f_getseters[] = {
    {"integration_type",
     (getter)integration_type_get,
     (setter)integration_type_set,
     integration_type_doc,
     nullptr},
    {nullptr, nullptr, nullptr, nullptr, nullptr} /* Sentinel */
};

/*-----------------------BPy_UnaryFunction1DVec3f type definition ------------------------------*/

PyTypeObject UnaryFunction1DVec3f_Type = {
    PyVarObject_HEAD_INIT(nullptr, 0) "UnaryFunction1DVec3f", /* tp_name */
    sizeof(BPy_UnaryFunction1DVec3f),                         /* tp_basicsize */
    0,                                                        /* tp_itemsize */
    (destructor)UnaryFunction1DVec3f___dealloc__,             /* tp_dealloc */
    0,                                                        /* tp_vectorcall_offset */
    nullptr,                                                  /* tp_getattr */
    nullptr,                                                  /* tp_setattr */
    nullptr,                                                  /* tp_reserved */
    (reprfunc)UnaryFunction1DVec3f___repr__,                  /* tp_repr */
    nullptr,                                                  /* tp_as_number */
    nullptr,                                                  /* tp_as_sequence */
    nullptr,                                                  /* tp_as_mapping */
    nullptr,                                                  /* tp_hash */
    (ternaryfunc)UnaryFunction1DVec3f___call__,               /* tp_call */
    nullptr,                                                  /* tp_str */
    nullptr,                                                  /* tp_getattro */
    nullptr,                                                  /* tp_setattro */
    nullptr,                                                  /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,                 /* tp_flags */
    UnaryFunction1DVec3f___doc__,                             /* tp_doc */
    nullptr,                                                  /* tp_traverse */
    nullptr,                                                  /* tp_clear */
    nullptr,                                                  /* tp_richcompare */
    0,                                                        /* tp_weaklistoffset */
    nullptr,                                                  /* tp_iter */
    nullptr,                                                  /* tp_iternext */
    nullptr,                                                  /* tp_methods */
    nullptr,                                                  /* tp_members */
    BPy_UnaryFunction1DVec3f_getseters,                       /* tp_getset */
    &UnaryFunction1D_Type,                                    /* tp_base */
    nullptr,                                                  /* tp_dict */
    nullptr,                                                  /* tp_descr_get */
    nullptr,                                                  /* tp_descr_set */
    0,                                                        /* tp_dictoffset */
    (initproc)UnaryFunction1DVec3f___init__,                  /* tp_init */
    nullptr,                                                  /* tp_alloc */
    nullptr,                                                  /* tp_new */
};

///////////////////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
}
#endif
