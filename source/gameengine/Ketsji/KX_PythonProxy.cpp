/**
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Contributor(s): Mitchell Stokes, Diego Lopes, Tristan Porteries.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

#include "KX_PythonProxy.h"

#include <fmt/format.h>

#include "BKE_python_proxy.hh"
#include "CM_Message.h"
#include "DNA_python_proxy_types.h"

KX_PythonProxy::KX_PythonProxy()
    : EXP_Value(),
      m_init(false),
      m_pp(nullptr),
#ifdef WITH_PYTHON
      m_update(nullptr),
      m_dispose(nullptr),
      m_logger(nullptr)
#endif
{
}

KX_PythonProxy::~KX_PythonProxy()
{
  Dispose();
}

std::string KX_PythonProxy::GetName()
{
  return m_pp->name;
}

PythonProxy *KX_PythonProxy::GetPrototype()
{
  return m_pp;
}

void KX_PythonProxy::SetPrototype(PythonProxy *pp)
{
  m_pp = pp;
}

void KX_PythonProxy::Start()
{
  if (!m_pp || m_init) {
    return;
  }
  else {
    m_init = true;
  }
#ifdef WITH_PYTHON
  PyObject *proxy = GetProxy();
  PyObject *arg_dict = (PyObject *)BKE_python_proxy_argument_dict_new(m_pp);

  if (PyObject_CallMethod(proxy, "start", "O", arg_dict)) {
    if (PyObject_HasAttrString(proxy, "update")) {
      m_update = PyObject_GetAttrString(proxy, "update");
    }

    if (PyObject_HasAttrString(proxy, "dispose")) {
      m_dispose = PyObject_GetAttrString(proxy, "dispose");
    }
  }

  if (PyErr_Occurred()) {
    LogError("Failed to invoke the start callback.");
  }

  Py_XDECREF(arg_dict);
  Py_XDECREF(proxy);
#endif
}

void KX_PythonProxy::Update()
{
  if (!m_pp) {
    return;
  }

  if (m_init) {
#ifdef WITH_PYTHON
    if (m_update && !PyObject_CallNoArgs(m_update) && PyErr_Occurred()) {
      LogError("Failed to invoke the update callback.");
    }
#endif
  }
  else {
    Start();
  }
}

KX_PythonProxy *KX_PythonProxy::GetReplica()
{
  KX_PythonProxy *replica = NewInstance();

  // this will copy properties and so on...
  replica->ProcessReplica();
#ifdef WITH_PYTHON
  PyTypeObject *type = Py_TYPE(GetProxy());

  if (!py_base_new(type, PyTuple_Pack(1, replica->GetProxy()), nullptr)) {
    CM_Error("Failed to replicate object: \"" << GetName() << "\"");
    delete replica;
    return nullptr;
  }
#endif
  return replica;
}

void KX_PythonProxy::ProcessReplica()
{
  EXP_Value::ProcessReplica();

  m_init = false;
#ifdef WITH_PYTHON
  m_update = nullptr;
  m_dispose = nullptr;
  m_logger = nullptr;
#endif
}

void KX_PythonProxy::Dispose()
{
#ifdef WITH_PYTHON
  if (m_dispose && !PyObject_CallNoArgs(m_dispose)) {
    LogError("Failed to invoke the dispose callback.");
  }

  Py_XDECREF(m_update);
  Py_XDECREF(m_dispose);
  Py_XDECREF(m_logger);

  m_update = nullptr;
  m_dispose = nullptr;
  m_logger = nullptr;
#endif
}

void KX_PythonProxy::LogError(const std::string &name)
{
#ifdef WITH_PYTHON
  PyObject *type, *value, *traceback;

  PyErr_Fetch(&type, &value, &traceback);
  PyErr_NormalizeException(&type, &value, &traceback);

  PyObject *logger = GetLogger();

  if (logger) {
    PyObject *msg = PyUnicode_FromStdString(name);
    PyObject *reporter = PyObject_GetAttrString(logger, "error");

    PyObject *args = PyTuple_New(1);
    PyObject *kwargs = PyDict_New();
    PyObject *exc_info = nullptr;

    if (value) {
      PyTuple_SetItem(args, 0, msg);

      exc_info = PyTuple_New(3);

      PyTuple_SetItem(exc_info, 0, type);
      PyTuple_SetItem(exc_info, 1, value);

      if (traceback) {
        PyTuple_SetItem(exc_info, 2, traceback);
      }
      else {
        PyTuple_SetItem(exc_info, 2, Py_None);
      }

      PyDict_SetItemString(kwargs, "exc_info", exc_info);
    }
    else {
      PyTuple_SetItem(args, 0, msg);
    }

    PyObject_Call(reporter, args, kwargs);

    Py_XDECREF(exc_info);

    Py_DECREF(args);
    Py_DECREF(kwargs);
    Py_DECREF(reporter);
  }
#endif
}

#ifdef WITH_PYTHON
PyObject *KX_PythonProxy::GetLogger()
{
  if (!m_logger) {
    PyObject *module = PyImport_GetModule(PyUnicode_FromStdString("logging"));

    if (module) {
      PyObject *proxy = GetProxy();
      PyObject *name = PyObject_GetAttrString(proxy, "loggerName");

      if (proxy && name) {
        m_logger = PyObject_CallMethod(module, "getLogger", "O", name);
      }

      Py_XDECREF(module);
    }

    if (PyErr_Occurred()) {
      PyErr_Print();
    }
  }

  return m_logger;
}

PyObject *KX_PythonProxy::pyattr_get_logger_name(EXP_PyObjectPlus *self_v,
                                                 const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_PythonProxy *self = static_cast<KX_PythonProxy *>(self_v);

  std::string repr = fmt::format("{}[{}]", self->GetType()->tp_name, self->GetText());

  return PyUnicode_FromStdString(repr);
}

PyObject *KX_PythonProxy::pyattr_get_logger(EXP_PyObjectPlus *self_v,
                                            const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_PythonProxy *self = static_cast<KX_PythonProxy *>(self_v);

  PyObject *logger = self->GetLogger();

  Py_XINCREF(logger);

  return logger;
}
#endif //WITH_PYTHON
