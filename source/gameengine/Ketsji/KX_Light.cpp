/*
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
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Ketsji/KX_Light.cpp
 *  \ingroup ketsji
 */

#ifdef _MSC_VER
#  pragma warning(disable : 4786)
#endif

#include "KX_Light.h"

KX_LightObject::KX_LightObject() : KX_GameObject(), m_obLight(nullptr)
{
}

KX_LightObject::~KX_LightObject()
{
}

void KX_LightObject::SetBlenderObject(Object *obj)
{
  KX_GameObject::SetBlenderObject(obj);

  m_light = static_cast<Light *>(obj->data);
}

Light *KX_LightObject::GetLight()
{
  return m_light;
}

KX_PythonProxy *KX_LightObject::NewInstance()
{
  return new KX_LightObject(*this);
}

void KX_LightObject::ProcessReplica()
{
  KX_GameObject::ProcessReplica();

  m_obLight = m_pBlenderObject;
  m_light = static_cast<Light *>(m_obLight->data);
}

#ifdef WITH_PYTHON
/* ------------------------------------------------------------------------- */
/* Python Integration Hooks					                                 */
/* ------------------------------------------------------------------------- */
PyObject *KX_LightObject::game_object_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
  KX_LightObject *obj = new KX_LightObject();

  PyObject *proxy = py_base_new(type, PyTuple_Pack(1, obj->GetProxy()), kwds);
  if (!proxy) {
    delete obj;
    return nullptr;
  }

  return proxy;
}

PyTypeObject KX_LightObject::Type = {PyVarObject_HEAD_INIT(nullptr, 0) "KX_LightObject",
                                     sizeof(EXP_PyObjectPlus_Proxy),
                                     0,
                                     py_base_dealloc,
                                     0,
                                     0,
                                     0,
                                     0,
                                     py_base_repr,
                                     0,
                                     &KX_GameObject::Sequence,
                                     &KX_GameObject::Mapping,
                                     0,
                                     0,
                                     0,
                                     nullptr,
                                     nullptr,
                                     0,
                                     Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
                                     0,
                                     0,
                                     0,
                                     0,
                                     0,
                                     0,
                                     0,
                                     Methods,
                                     0,
                                     0,
                                     &KX_GameObject::Type,
                                     0,
                                     0,
                                     0,
                                     0,
                                     0,
                                     0,
                                     game_object_new};

PyMethodDef KX_LightObject::Methods[] = {
    // EXP_PYMETHODTABLE_NOARGS(KX_LightObject, updateShadow),
    {nullptr, nullptr}  // Sentinel
};

PyAttributeDef KX_LightObject::Attributes[] = {
    // EXP_PYATTRIBUTE_RW_FUNCTION("energy", KX_LightObject, pyattr_get_energy, pyattr_set_energy),

    EXP_PYATTRIBUTE_NULL  // Sentinel
};
#endif  // WITH_PYTHON
