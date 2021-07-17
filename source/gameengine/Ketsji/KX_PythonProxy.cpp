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

#include "BKE_python_proxy.h"
#include "DNA_python_proxy_types.h"
#include "EXP_Value.h"

KX_PythonProxy::KX_PythonProxy():
    EXP_Value(),
    m_init(false),
    m_pp(nullptr),
    m_update(nullptr),
    m_dispose(nullptr)
{
}

KX_PythonProxy::~KX_PythonProxy()
{
  Dispose();
  Reset();
}

void KX_PythonProxy::SetPrototype(PythonProxy *pp)
{
  m_pp = pp;
}

void KX_PythonProxy::Start()
{
  if (!m_pp || m_init) {
    return;
  } else {
    m_init = true;
  }

  PyObject *proxy = GetProxy();
  PyObject *arg_dict = (PyObject *)BKE_python_proxy_argument_dict_new(m_pp);

  if (PyObject_CallMethod(proxy, "start", "O", arg_dict))
  {
    m_update = PyObject_GetAttrString(proxy, "update");
    m_dispose = PyObject_GetAttrString(proxy, "dispose");

    PyErr_Clear();
  } else {
    PyErr_Print();
  }

  Py_XDECREF(arg_dict);
  Py_XDECREF(proxy);
}

void KX_PythonProxy::Update()
{
  if (!m_pp) {
    return;
  }

  if (m_init) {
    if (m_update && !PyObject_CallNoArgs(m_update)) {
      PyErr_Print();
    }
  } else {
    Start();
  }
}

void KX_PythonProxy::Dispose()
{
  if (m_dispose && !PyObject_CallNoArgs(m_dispose)) {
    PyErr_Print();
  }
}

void KX_PythonProxy::Reset()
{
  if (m_update) {
    Py_DECREF(m_update);
  }

  if (m_dispose) {
    Py_DECREF(m_dispose);
  }

  m_init = false;
}
