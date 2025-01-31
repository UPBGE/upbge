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

/** \file gameengine/Ketsji/KX_FontObject.cpp
 *  \ingroup ketsji
 */

#include "KX_FontObject.h"

#include "BLI_string_utf8.h"
#include "DNA_curve_types.h"
#include "MEM_guardedalloc.h"

static std::vector<std::string> split_string(std::string str)
{
  std::vector<std::string> text = std::vector<std::string>();

  // Split the string upon new lines
  int begin = 0, end = 0;
  while (end < str.size()) {
    if (str[end] == '\n') {
      text.push_back(str.substr(begin, end - begin));
      begin = end + 1;
    }
    end++;
  }
  // Now grab the last line
  text.push_back(str.substr(begin, end - begin));

  return text;
}

KX_FontObject::KX_FontObject() : KX_GameObject(), m_object(nullptr), m_rasterizer(nullptr)
{
}

KX_FontObject::~KX_FontObject()
{
  // remove font from the scene list
  // it's handled in KX_Scene::NewRemoveObject
  UpdateCurveText(m_backupText);  // eevee
}

KX_PythonProxy *KX_FontObject::NewInstance()
{
  return new KX_FontObject(*this);
}

void KX_FontObject::ProcessReplica()
{
  KX_GameObject::ProcessReplica();
}

void KX_FontObject::SetText(const std::string &text)
{
  m_text = text;
  m_texts = split_string(text);
}

void KX_FontObject::UpdateCurveText(std::string newText)  // eevee
{
  Object *ob = GetBlenderObject();
  Curve *cu = (Curve *)ob->data;
  if (cu->str)
    MEM_freeN(cu->str);
  if (cu->strinfo)
    MEM_freeN(cu->strinfo);

  size_t len_bytes;
  size_t len_chars = BLI_strlen_utf8_ex(newText.c_str(), &len_bytes);

  cu->len_char32 = len_chars;
  cu->len = len_bytes;
  cu->strinfo = static_cast<CharInfo *>(MEM_callocN((len_chars + 1) * sizeof(CharInfo), __func__));
  cu->str = static_cast<char *>(MEM_mallocN(len_bytes + sizeof(char32_t), __func__));
  memcpy(cu->str, newText.c_str(), len_bytes + 1);

  if (ob->gameflag & OB_OVERLAY_COLLECTION) {
    GetScene()->AppendToIdsToUpdateInOverlayPass(&ob->id, ID_RECALC_GEOMETRY);
  }
  else {
    GetScene()->AppendToIdsToUpdateInAllRenderPasses(&ob->id, ID_RECALC_GEOMETRY);
  }
}

void KX_FontObject::UpdateTextFromProperty()
{
  // Allow for some logic brick control
  EXP_Value *prop = GetProperty("Text");
  if (prop && prop->GetText() != m_text) {
    SetText(prop->GetText());
    UpdateCurveText(m_text);  // eevee
  }
}

void KX_FontObject::SetRasterizer(RAS_Rasterizer *rasterizer)
{
  m_rasterizer = rasterizer;
}

void KX_FontObject::SetBlenderObject(Object *obj)
{
  KX_GameObject::SetBlenderObject(obj);

  if (obj) {
    Curve *text = static_cast<Curve *>(obj->data);

    m_backupText = std::string(text->str);  // eevee

    SetText(text->str);
  }
}

#ifdef WITH_PYTHON

/* ------------------------------------------------------------------------- */
/* Python Integration Hooks					                                 */
/* ------------------------------------------------------------------------- */
PyObject *KX_FontObject::game_object_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
  KX_FontObject *obj = new KX_FontObject();

  PyObject *proxy = py_base_new(type, PyTuple_Pack(1, obj->GetProxy()), kwds);
  if (!proxy) {
    delete obj;
    return nullptr;
  }

  return proxy;
}

PyTypeObject KX_FontObject::Type = {PyVarObject_HEAD_INIT(nullptr, 0) "KX_FontObject",
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

PyMethodDef KX_FontObject::Methods[] = {
    {nullptr, nullptr}  // Sentinel
};

PyAttributeDef KX_FontObject::Attributes[] = {
    EXP_PYATTRIBUTE_NULL  // Sentinel
};

#endif  // WITH_PYTHON
