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

#include "KX_PythonLogger.h"
#include "EXP_Value.h"

#include <boost/format.hpp>

KX_PythonLogger::KX_PythonLogger():
    EXP_Value(),
    m_logger(nullptr)
{
}

KX_PythonLogger::~KX_PythonLogger()
{
  Py_XDECREF(m_logger);

  m_logger = nullptr;
}

PyObject *KX_PythonLogger::GetLogger()
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

void KX_PythonLogger::LogError(const std::string &name)
{
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

    if (value)
    {
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
    else
    {
      PyTuple_SetItem(args, 0, msg);
    }

    PyObject_Call(reporter, args, kwargs);

    Py_XDECREF(exc_info);

    Py_DECREF(args);
    Py_DECREF(kwargs);
    Py_DECREF(reporter);
  }
}

void KX_PythonLogger::ProcessReplica()
{
  EXP_Value::ProcessReplica();

  Py_XDECREF(m_logger);

  m_logger = nullptr;
}

PyObject *KX_PythonLogger::pyattr_get_logger_name(EXP_PyObjectPlus *self_v,
                                                  const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_PythonLogger *self = static_cast<KX_PythonLogger *>(self_v);

  std::string repr = (boost::format("%s[%s]") % self->GetType()->tp_name % self->GetText()).str();

  return PyUnicode_FromStdString(repr);
}

PyObject *KX_PythonLogger::pyattr_get_logger(EXP_PyObjectPlus *self_v,
                                             const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_PythonLogger *self = static_cast<KX_PythonLogger *>(self_v);

  PyObject *logger = self->GetLogger();

  Py_XINCREF(logger);

  return logger;
}
