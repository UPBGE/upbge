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

#include "BLI_blenlib.h"
#include "DNA_curve_types.h"
#include "MEM_guardedalloc.h"
#include "depsgraph/DEG_depsgraph.h"

#include "EXP_StringValue.h"

#define MAX_BGE_TEXT_LEN 1024  // eevee

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

KX_FontObject::KX_FontObject(void *sgReplicationInfo,
                             SG_Callbacks callbacks,
                             RAS_Rasterizer *rasterizer,
                             Object *ob)
    : KX_GameObject(sgReplicationInfo, callbacks), m_object(ob), m_rasterizer(rasterizer)
{
  Curve *text = static_cast<Curve *>(ob->data);

  SetText(text->str);

  m_backupText = std::string(text->str);  // eevee
}

KX_FontObject::~KX_FontObject()
{
  // remove font from the scene list
  // it's handled in KX_Scene::NewRemoveObject
  UpdateCurveText(m_backupText);  // eevee
}

CValue *KX_FontObject::GetReplica()
{
  KX_FontObject *replica = new KX_FontObject(*this);
  replica->ProcessReplica();
  return replica;
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

  cu->len_char32 = strlen(newText.c_str());
  cu->len = BLI_strlen_utf8(newText.c_str());
  cu->strinfo = (CharInfo *)MEM_callocN((cu->len_char32 + 4) * sizeof(CharInfo), "texteditinfo");
  cu->str = (char *)MEM_mallocN(cu->len + sizeof(wchar_t), "str");
  BLI_strncpy(cu->str, newText.c_str(), MAX_BGE_TEXT_LEN);

  DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
  DEG_id_tag_update(&ob->id, ID_RECALC_COPY_ON_WRITE);

  GetScene()->ResetTaaSamples();
}

void KX_FontObject::UpdateTextFromProperty()
{
  // Allow for some logic brick control
  CValue *prop = GetProperty("Text");
  if (prop && prop->GetText() != m_text) {
    SetText(prop->GetText());
    UpdateCurveText(m_text);  // eevee
  }
}

#ifdef WITH_PYTHON

/* ------------------------------------------------------------------------- */
/* Python Integration Hooks					                                 */
/* ------------------------------------------------------------------------- */

PyTypeObject KX_FontObject::Type = {PyVarObject_HEAD_INIT(nullptr, 0) "KX_FontObject",
                                    sizeof(PyObjectPlus_Proxy),
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
                                    py_base_new};

PyMethodDef KX_FontObject::Methods[] = {
    {nullptr, nullptr}  // Sentinel
};

PyAttributeDef KX_FontObject::Attributes[] = {
    KX_PYATTRIBUTE_NULL  // Sentinel
};

#endif  // WITH_PYTHON
